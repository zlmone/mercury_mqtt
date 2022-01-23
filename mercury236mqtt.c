#include <sys/types.h>
#include <sys/stat.h>
#include <sys/select.h>
#include <fcntl.h>
#include <termios.h>
#include <stdio.h>
#include <strings.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <stdint.h>
#include <math.h>
#include <time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <stdio.h>
#include <signal.h>

#include<mosquitto.h>

#include "util.h"
#include "m_enums.h"
//#include "m_struct.h"

#pragma pack(1)

#define BAUDRATE 	B9600		// 9600 baud
#define _POSIX_SOURCE 	1		// POSIX compliant source
#define UInt16		uint16_t
#define byte		unsigned char
#define TIME_OUT	50 * 1000	// Mercury inter-command delay (mks)
#define CH_TIME_OUT	2		// Channel timeout (sec)
#define BSZ		255
#define PM_ADDRESS	0		// RS485 addess of the power meter
#define TARRIF_NUM	2		// 2 tariffs supported
#define OPT_DEBUG	"--debug"
#define OPT_HELP	"--help"

#define MQTT_SERVER	"10.32.0.5"
#define MQTT_USER	""
#define MQTT_PASSWORD	""
#define LOOP_DELAY	5 //seconds
#define SKIP_SEND	10

static volatile int keepRunning = 1;
int debugPrint = 0;
int units_get = 100;
char dt[32];
FILE *f_log;

void intHandler(int dummy) {
	keepRunning = 0;
}

struct mosquitto *mosq;


// ***** Commands
// Test connection
typedef struct {
 	byte	address;
 	byte	command;
 	UInt16	CRC;
} TestCmd;

// Connection initialisaton command
typedef struct {
 	byte	address;
 	byte	command;
 	byte 	accessLevel;
 	byte	password[6];
 	UInt16	CRC;
} InitCmd;

// Connection terminaion command
typedef struct {
 	byte	address;
 	byte	command;
 	UInt16	CRC;
} ByeCmd;

// Power meter parameters read command
typedef struct {
 	byte	address;
	byte	command;	// 8h
	byte	paramId;	// No of parameter to read
	byte	BWRI;
	UInt16 	CRC;
} ReadParamCmd;


typedef struct {
 	byte	address;
	byte	command;	// 8h
	byte	paramId;	// No of parameter to read
	UInt16 	CRC;
} ReadParamCmd2;


// ***** Results
// 1-byte responce (usually with status code)
typedef struct {
	byte	address;
	byte	result;
	UInt16	CRC;
} Result_1b;

// 3-byte responce
typedef struct {
	byte	address;
	byte	res[3];
	UInt16	CRC;
} Result_3b;

// Result with 3 bytes per phase
typedef struct {
	byte	address;
	byte	p1[3];
	byte	p2[3];
	byte	p3[3];
	UInt16	CRC;
} Result_3x3b;

// Result with 3 bytes per phase plus 3 bytes for phases sum
typedef struct {
	byte	address;
	byte	sum[3];
	byte	p1[3];
	byte	p2[3];
	byte	p3[3];
	UInt16	CRC;
} Result_4x3b;

// Result with 4 bytes per phase plus 4 bytes for sum
typedef struct {
	byte	address;
	byte	ap[4];		// active +
	byte	am[4];		// active -
	byte	rp[4];		// reactive +
	byte	rm[4];		// reactive -
	UInt16	CRC;
} Result_4x4b;

// 3-phase vector (for voltage, frequency, power by phases)
typedef struct {
	float	p1;
	float	p2;
	float	p3;
} P3V;

// 3-phase vector (for voltage, frequency, power by phases) with sum by all phases
typedef struct {
	float	sum;
	float	p1;
	float	p2;
	float	p3;
} P3VS;

// Power vector
typedef struct {
	float 	ap;		// active +
	float	am;		// active -
	float 	rp;		// reactive +
	float 	rm;		// reactive -
} PWV;

// Output results block
typedef struct {
	P3V 	U;			// voltage
	P3V	I;			// current
	P3V	A;			// phase angles
	P3VS	C;			// cos(f)
	P3VS	P;			// current active power consumption
	P3VS	S;			// current reactive power consumption
	PWV	PR;			// power counters from reset (all tariffs)
	PWV	PRT[TARRIF_NUM];	// power counters from reset (by tariffs)
	PWV	PY;			// power counters for yesterday
	PWV	PT;			// power counters for today
	float	f;			// grid frequency
} OutputBlock;


OutputBlock o;


UInt16 ModRTU_CRC(byte* buf, int len) {
	UInt16 crc = 0xFFFF;

	for (int pos = 0; pos < len; pos++) {
		crc ^= (UInt16)buf[pos];          // XOR byte into least sig. byte of crc
		for (int i = 8; i != 0; i--) {    // Loop over each bit
			if ((crc & 0x0001) != 0) {      // If the LSB is set
				crc >>= 1;                    // Shift right and XOR 0xA001
				crc ^= 0xA001;
			} else {                           // Else LSB is not set
			        crc >>= 1;                    // Just shift right
			}
		}
	}
	return crc;
}


void sendMQTT(const char* topic, const float value) {
	int rc = MOSQ_ERR_SUCCESS;

	char msg[32];
	sprintf(msg,"%.2f", value);
	rc = mosquitto_publish(mosq, NULL, topic, strlen(msg), msg, 0, 0);

	if(rc){
		switch(rc){
			case MOSQ_ERR_INVAL:
				fprintf(stderr, "Error: Invalid input. Does your topic contain '+' or '#'?\n");
				break;
			case MOSQ_ERR_NOMEM:
				fprintf(stderr, "Error: Out of memory when trying to publish message.\n");
				break;
			case MOSQ_ERR_NO_CONN:
				fprintf(stderr, "Error: Client not connected when trying to publish.\n");
				fprintf(stderr, "Reconnecting...\n");
				mosquitto_connect(mosq, MQTT_SERVER, 1883, 60);
				int status = mosquitto_max_inflight_messages_set(mosq, 32);
				if (status) {
					printf("Error (%d) setting max inflight message count.\n", status);
				}

				break;
			case MOSQ_ERR_PROTOCOL:
				fprintf(stderr, "Error: Protocol error when communicating with broker.\n");
				break;
			case MOSQ_ERR_PAYLOAD_SIZE:
				fprintf(stderr, "Error: Message payload is too large.\n");
				break;
		}
	}
}


	// -- Abnormal termination
void exitFailure(const char* msg) {
	perror(msg);
	exit(EXIT_FAIL);
}


int nb_read_impl(int fd, byte* buf, int sz) {
	fd_set set;
	struct timeval timeout;

		// Initialise the input set
	FD_ZERO(&set);
	FD_SET(fd, &set);

		// Set timeout
	timeout.tv_sec = CH_TIME_OUT;
	timeout.tv_usec = 0;

	int r = select(fd + 1, &set, NULL, NULL, &timeout);
	if (r < 0)
		exitFailure("Select failed.");
	if (r == 0)
		return 0;

	return read(fd, buf, BSZ);
}

	// -- Non-blocking file read with timeout
	// -- Aborts if timed out.
int nb_read(int fd, byte* buf, int sz)
{
	int r = nb_read_impl(fd, buf, sz);
	if (r == 0)
		exitFailure("Communication channel timeout.");
	return r;
}

	// -- Check 1 byte responce
int checkResult_1b(byte* buf, int len)
{
	if (len != sizeof(Result_1b))
		return WRONG_RESULT_SIZE;

	Result_1b *res = (Result_1b*)buf;
	UInt16 crc = ModRTU_CRC(buf, len - sizeof(UInt16));
	if (crc != res->CRC)
		return WRONG_CRC;

	return res->result & 0x0F;
}

	// -- Check 3 byte responce
int checkResult_3b(byte* buf, int len)
{
	if (len != sizeof(Result_3b))
		return WRONG_RESULT_SIZE;

	Result_3b *res = (Result_3b*)buf;
	UInt16 crc = ModRTU_CRC(buf, len - sizeof(UInt16));
	if (crc != res->CRC)
		return WRONG_CRC;

	return OK;
}

	// -- Check 3 bytes x 3 phase responce
int checkResult_3x3b(byte* buf, int len)
{
	if (len != sizeof(Result_3x3b))
		return WRONG_RESULT_SIZE;

	Result_3x3b *res = (Result_3x3b*)buf;
	UInt16 crc = ModRTU_CRC(buf, len - sizeof(UInt16));
	if (crc != res->CRC)
		return WRONG_CRC;

	return OK;
}

	// -- Check 3 bytes x 3 phase and sum responce
int checkResult_4x3b(byte* buf, int len)
{
	if (len != sizeof(Result_4x3b))
		return WRONG_RESULT_SIZE;

	Result_4x3b *res = (Result_4x3b*)buf;
	UInt16 crc = ModRTU_CRC(buf, len - sizeof(UInt16));
	if (crc != res->CRC)
		return WRONG_CRC;

	return OK;
}

	// -- Check 4 bytes x 3 phase and sum responce
int checkResult_4x4b(byte* buf, int len)
{
	if (len != sizeof(Result_4x4b))
		return WRONG_RESULT_SIZE;

	Result_4x4b *res = (Result_4x4b*)buf;
	UInt16 crc = ModRTU_CRC(buf, len - sizeof(UInt16));
	if (crc != res->CRC)
		return WRONG_CRC;

	return OK;
}

	// -- Check the communication channel
int checkChannel(int ttyd)
{
		// Command initialisation
	TestCmd testCmd = { .address = PM_ADDRESS, .command = 0x00 };
	testCmd.CRC = ModRTU_CRC((byte*)&testCmd, sizeof(testCmd) - sizeof(UInt16));

		// Send test channel command
	write(ttyd, (byte*)&testCmd, sizeof(testCmd));
	usleep(TIME_OUT);

		// Get responce
	byte buf[BSZ];
	int len = nb_read_impl(ttyd, buf, BSZ);
	if (len == 0)
		return CHECK_CHANNEL_TIME_OUT;


	return checkResult_1b(buf, len);
}

	// -- Connection initialisation
int initConnection(int ttyd)
{
	InitCmd initCmd = {
		.address = PM_ADDRESS,
		.command = 0x01,
		.accessLevel = 0x01,
		.password = { 0x01, 0x01, 0x01, 0x01, 0x01, 0x01 },
	};
	initCmd.CRC = ModRTU_CRC((byte*)&initCmd, sizeof(initCmd) - sizeof(UInt16));

	write(ttyd, (byte*)&initCmd, sizeof(initCmd));
	usleep(TIME_OUT);

		// Read initialisation result
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);

	return checkResult_1b(buf, len);
}

	// -- Close connection
int closeConnection(int ttyd)
{
	ByeCmd byeCmd = { .address = PM_ADDRESS, .command = 0x02 };
	byeCmd.CRC = ModRTU_CRC((byte*)&byeCmd, sizeof(byeCmd) - sizeof(UInt16));

	write(ttyd, (byte*)&byeCmd, sizeof(byeCmd));
	usleep(TIME_OUT);

		// Read closing responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);

	return checkResult_1b(buf, len);
}

	// Decode float from 3 bytes
float B3F(byte b[3], float factor)
{
	int val = ((b[0] & 0x3F) << 16) | (b[2] << 8) | b[1];
	return val/factor;
}

	// Decode float from 4 bytes
float B4F(byte b[4], float factor)
{
	int val = ((b[1] & 0x3F) << 24) | (b[0] << 16) | (b[3] << 8) | b[2];
	return val/factor;
}



void printDateTime(byte *data, int size)
{
	sprintf(dt, "20%02X-%02X-%02X %02X:%02X:%02X", (byte)data[7], (byte)data[6], (byte)data[5], (byte)data[3], (byte)data[2], (byte)data[1]);
}

int getTime(int ttyd)
{
	ReadParamCmd2 getFCmd =
	{
		.address = PM_ADDRESS,
		.command = 0x04,
		.paramId = 0x0
	};
	getFCmd.CRC = ModRTU_CRC((byte*)&getFCmd, sizeof(getFCmd) - sizeof(UInt16));

	write(ttyd, (byte*)&getFCmd, sizeof(getFCmd));
	usleep(TIME_OUT);

		// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);
	printDateTime((byte*)buf, len);

	sprintf(dt, "20%02X-%02X-%02X %02X:%02X:%02X", (byte)buf[7], (byte)buf[6], (byte)buf[5], (byte)buf[3], (byte)buf[2], (byte)buf[1]);

	return 0;// checkResult;
}







	// Get voltage (U) by phases
int getU(int ttyd, P3V* U)
{
	ReadParamCmd getUCmd =
	{
		.address = PM_ADDRESS,
		.command = 0x08,
		.paramId = 0x16,
		.BWRI = 0x11
	};
	getUCmd.CRC = ModRTU_CRC((byte*)&getUCmd, sizeof(getUCmd) - sizeof(UInt16));

	write(ttyd, (byte*)&getUCmd, sizeof(getUCmd));
	usleep(TIME_OUT);

		// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);

		// Check and decode result
	int checkResult = checkResult_3x3b(buf, len);
	if (OK == checkResult)
	{
		Result_3x3b* res = (Result_3x3b*)buf;
		U->p1 = B3F(res->p1, 100.0);
		U->p2 = B3F(res->p2, 100.0);
		U->p3 = B3F(res->p3, 100.0);
	}

	return checkResult;
}

	// Get current (I) by phases
int getI(int ttyd, P3V* I)
{
	ReadParamCmd getICmd =
	{
		.address = PM_ADDRESS,
		.command = 0x08,
		.paramId = 0x16,
		.BWRI = 0x21
	};
	getICmd.CRC = ModRTU_CRC((byte*)&getICmd, sizeof(getICmd) - sizeof(UInt16));

	write(ttyd, (byte*)&getICmd, sizeof(getICmd));
	usleep(TIME_OUT);

		// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);

		// Check and decode result
	int checkResult = checkResult_3x3b(buf, len);
	if (OK == checkResult)
	{
		Result_3x3b* res = (Result_3x3b*)buf;
		I->p1 = B3F(res->p1, 1000.0);
		I->p2 = B3F(res->p2, 1000.0);
		I->p3 = B3F(res->p3, 1000.0);
	}

	return checkResult;
}

	// Get power consumption factor cos(f) by phases
int getCosF(int ttyd, P3VS* C)
{
	ReadParamCmd getCosCmd =
	{
		.address = PM_ADDRESS,
		.command = 0x08,
		.paramId = 0x16,
		.BWRI = 0x30
	};
	getCosCmd.CRC = ModRTU_CRC((byte*)&getCosCmd, sizeof(getCosCmd) - sizeof(UInt16));

	write(ttyd, (byte*)&getCosCmd, sizeof(getCosCmd));
	usleep(TIME_OUT);

		// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);

		// Check and decode result
	int checkResult = checkResult_4x3b(buf, len);
	if (OK == checkResult)
	{
		Result_4x3b* res = (Result_4x3b*)buf;
		C->p1 = B3F(res->p1, 1000.0);
		C->p2 = B3F(res->p2, 1000.0);
		C->p3 = B3F(res->p3, 1000.0);
		C->sum = B3F(res->sum, 1000.0);
	}

	return checkResult;
}

	// Get grid frequency (Hz)
int getF(int ttyd, float *f)
{
	ReadParamCmd getFCmd =
	{
		.address = PM_ADDRESS,
		.command = 0x08,
		.paramId = 0x16,
		.BWRI = 0x40
	};
	getFCmd.CRC = ModRTU_CRC((byte*)&getFCmd, sizeof(getFCmd) - sizeof(UInt16));

	write(ttyd, (byte*)&getFCmd, sizeof(getFCmd));
	usleep(TIME_OUT);

		// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);

		// Check and decode result
	int checkResult = checkResult_3b(buf, len);
	if (OK == checkResult)
	{
		Result_3b* res = (Result_3b*)buf;
		*f = B3F(res->res, 100.0);
	}

	return checkResult;
}

	// Get phases angle
int getA(int ttyd, P3V* A)
{
	ReadParamCmd getACmd =
	{
		.address = PM_ADDRESS,
		.command = 0x08,
		.paramId = 0x16,
		.BWRI = 0x51
	};
	getACmd.CRC = ModRTU_CRC((byte*)&getACmd, sizeof(getACmd) - sizeof(UInt16));

	write(ttyd, (byte*)&getACmd, sizeof(getACmd));
	usleep(TIME_OUT);

		// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);

		// Check and decode result
	int checkResult = checkResult_3x3b(buf, len);
	if (OK == checkResult)
	{
		Result_3x3b* res = (Result_3x3b*)buf;
		A->p1 = B3F(res->p1, 100.0);
		A->p2 = B3F(res->p2, 100.0);
		A->p3 = B3F(res->p3, 100.0);
	}

	return checkResult;
}

	// Get active power (W) consumption by phases with total
int getP(int ttyd, P3VS* P)
{
	ReadParamCmd getPCmd =
	{
		.address = PM_ADDRESS,
		.command = 0x08,
		.paramId = 0x16,
		.BWRI = 0x00
	};
	getPCmd.CRC = ModRTU_CRC((byte*)&getPCmd, sizeof(getPCmd) - sizeof(UInt16));

	write(ttyd, (byte*)&getPCmd, sizeof(getPCmd));
	usleep(TIME_OUT);

		// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);

		// Check and decode result
	int checkResult = checkResult_4x3b(buf, len);
	if (OK == checkResult)
	{
		Result_4x3b* res = (Result_4x3b*)buf;
		P->p1 = B3F(res->p1, 100.0);
		P->p2 = B3F(res->p2, 100.0);
		P->p3 = B3F(res->p3, 100.0);
		P->sum = B3F(res->sum, 100.0);
	}

	return checkResult;
}

	// Get reactive power (VA) consumption by phases with total
int getS(int ttyd, P3VS* S)
{
	ReadParamCmd getSCmd =
	{
		.address = PM_ADDRESS,
		.command = 0x08,
		.paramId = 0x16,
		.BWRI = 0x08
	};
	getSCmd.CRC = ModRTU_CRC((byte*)&getSCmd, sizeof(getSCmd) - sizeof(UInt16));

	write(ttyd, (byte*)&getSCmd, sizeof(getSCmd));
	usleep(TIME_OUT);

		// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);

		// Check and decode result
	int checkResult = checkResult_4x3b(buf, len);
	if (OK == checkResult)
	{
		Result_4x3b* res = (Result_4x3b*)buf;
		S->p1 = B3F(res->p1, 100.0);
		S->p2 = B3F(res->p2, 100.0);
		S->p3 = B3F(res->p3, 100.0);
		S->sum = B3F(res->sum, 100.0);
	}

	return checkResult;
}

	/* Get power counters by phases for the period
		periodId - one of PowerPeriod enum values
		month - month number when periodId is PP_MONTH
		tariffNo - 0 for all tariffs, 1 - tariff #1, 2 - tariff #2 etc. */
int getW(int ttyd, PWV* W, int periodId, int month, int tariffNo) {
	ReadParamCmd getWCmd = {
		.address = PM_ADDRESS,
		.command = 0x05,
		.paramId = (periodId << 4) | (month & 0xF),
		.BWRI = tariffNo
	};
	getWCmd.CRC = ModRTU_CRC((byte*)&getWCmd, sizeof(getWCmd) - sizeof(UInt16));

	write(ttyd, (byte*)&getWCmd, sizeof(getWCmd));
	usleep(TIME_OUT);

	// Read responce
	byte buf[BSZ];
	int len = nb_read(ttyd, buf, BSZ);

	// Check and decode result
	int checkResult = checkResult_4x4b(buf, len);
	if (OK == checkResult) {
		Result_4x4b* res = (Result_4x4b*)buf;
		W->ap = B4F(res->ap, 1000.0);
		W->am = B4F(res->am, 1000.0);
		W->rp = B4F(res->rp, 1000.0);
		W->rm = B4F(res->rm, 1000.0);
	}

	return checkResult;
}

void printUsage() {
	printf("Usage: mercury236 RS485  ...\n\r");
	printf("  RS485\t\taddress of RS485 dongle (e.g. /dev/ttyUSB0), required\n\r");
}

void init_mqtt() {
	mosquitto_lib_init();
	mosq = mosquitto_new(NULL, true, NULL);
	mosquitto_username_pw_set(mosq, MQTT_USER, MQTT_PASSWORD);

	mosquitto_connect(mosq, MQTT_SERVER, 1883, 60);

	int status = mosquitto_max_inflight_messages_set(mosq, 128);
	if (status) {
		printf("Error (%d) setting max inflight message count.\n", status);
	}
	sendMQTT("mercury/status", 1);

}

void send_base_mqtt() {
	fprintf(f_log, "MQTT: send...");
	sendMQTT("mercury/sensor/p1/u", round(o.U.p1));
	sendMQTT("mercury/sensor/p2/u", round(o.U.p2));
	sendMQTT("mercury/sensor/p3/u", round(o.U.p3));

	sendMQTT("mercury/sensor/p1/current", o.I.p1);
	sendMQTT("mercury/sensor/p2/current", o.I.p2);
	sendMQTT("mercury/sensor/p3/current", o.I.p3);

	sendMQTT("mercury/sensor/p1/cos", o.C.p1);
	sendMQTT("mercury/sensor/p2/cos", o.C.p2);
	sendMQTT("mercury/sensor/p3/cos", o.C.p3);

	sendMQTT("mercury/sensor/p1/angle", o.A.p1);
	sendMQTT("mercury/sensor/p2/angle", o.A.p2);
	sendMQTT("mercury/sensor/p3/angle", o.A.p3);

	sendMQTT("mercury/sensor/p1/reactive", o.S.p1);
	sendMQTT("mercury/sensor/p2/reactive", o.S.p2);
	sendMQTT("mercury/sensor/p3/reactive", o.S.p3);

	sendMQTT("mercury/sensor/p1/p", round(o.P.p1));
	sendMQTT("mercury/sensor/p2/p", round(o.P.p2));
	sendMQTT("mercury/sensor/p3/p", round(o.P.p3));

	sendMQTT("mercury/sensor/summary/frequency", o.f);

	sendMQTT("mercury/sensor/summary/total", round(o.PR.ap));
	sendMQTT("mercury/sensor/summary/today", o.PT.ap);
	fprintf(f_log, "OK\n");
}

// -- Output formatting and print
void printOutput() {
	// getting current time for timestamp
	char timeStamp[BSZ];
	getDateTimeStr(timeStamp, BSZ, time(NULL));
	printf("  Voltage (V):             		%8.2f %8.2f %8.2f\n\r", o.U.p1, o.U.p2, o.U.p3);
	printf("  Current (A):             		%8.2f %8.2f %8.2f\n\r", o.I.p1, o.I.p2, o.I.p3);
	printf("  Cos(f):                  		%8.2f %8.2f %8.2f (%8.2f)\n\r", o.C.p1, o.C.p2, o.C.p3, o.C.sum);
	printf("  Frequency (Hz):          		%8.2f\n\r", o.f);
	printf("  Phase angles (deg):      		%8.2f %8.2f %8.2f\n\r", o.A.p1, o.A.p2, o.A.p3);
	printf("  Active power (W):        		%8.2f %8.2f %8.2f (%8.2f)\n\r", o.P.p1, o.P.p2, o.P.p3, o.P.sum);
	printf("  Reactive power (VA):     		%8.2f %8.2f %8.2f (%8.2f)\n\r", o.S.p1, o.S.p2, o.S.p3, o.S.sum);
	printf("  Total consumed, KW:			%8.2f\n\r", o.PR.ap);
	printf("  Today consumed (KW):     		%8.2f\n\r", o.PT.ap);
}

float last_p1 = 0;
float last_p2 = 0;
float last_p3 = 0;

float last_va1 = 0;
float last_va2 = 0;
float last_va3 = 0;


int fd;
struct termios oldtio, newtio;
char dev[BSZ];



int main(int argc, const char** args) {
	if (argc < 2) {
		printf("Error: no RS485 device specified\n\r\n\r");
		printUsage();
		exit(EXIT_FAIL);
	}
	strncpy(dev, args[1], BSZ);

	for (int i=2; i<argc; i++) {
		if (!strcmp(OPT_DEBUG, args[i])) {
			debugPrint = 1;
		} else if (!strcmp(OPT_HELP, args[i])) {
			printUsage();
			exit(EXIT_OK);
		} else {
			printf("Error: %s option is not recognised\n\r\n\r", args[i]);
			printUsage();
			exit(EXIT_FAIL);
		}
	}

	bzero(&o, sizeof(o));

	fd = open(dev, O_RDWR | O_NOCTTY | O_NDELAY);
	if (fd < 0) {
		exitFailure(dev);
	}

	fcntl(fd, F_SETFL, 0);
	tcgetattr(fd, &oldtio); /* save current port settings */
	bzero(&newtio, sizeof(newtio));
	cfsetispeed(&newtio, BAUDRATE);
	cfsetospeed(&newtio, BAUDRATE);
	newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
	newtio.c_iflag = IGNPAR;
	newtio.c_oflag = 0;

	cfmakeraw(&newtio);
	tcsetattr(fd, TCSANOW, &newtio);

	init_mqtt();
	
	signal(SIGINT, intHandler);
	
	int ch = checkChannel(fd);
	if (ch != OK) {
		exitFailure("Power meter communication channel test failed.");
	}


	if (OK != initConnection(fd))
		exitFailure("Power meter connection initialisation error.");
	
	f_log = fopen("power_log.csv", "a");

	while (keepRunning) {
//		if (OK != getTime(fd))
//			exitFailure("Cannot collect date-time.");

		if (OK != getU(fd, &o.U)) {
			printf("U err\n");
		}

		if (OK != getI(fd, &o.I)) {
			printf("I err\n");
		}

		if (OK != getCosF(fd, &o.C)) {
			printf("C err\n");
		}

		if (OK != getF(fd, &o.f)) {
			printf("f err\n");
		}

		if (OK != getA(fd, &o.A)) {
			printf("A err\n");
		}

		if (OK != getP(fd, &o.P)) {
			printf("P err\n");
		}

		if (OK != getS(fd, &o.S)) {
			printf("S err\n");
		}

		if (OK != getW(fd, &o.PR, PP_RESET, 0, 0) ||		// total from reset
		    OK != getW(fd, &o.PT, PP_TODAY, 0, 0)) {
			printf("W error\n");
		}

		if (debugPrint) {
			printOutput(o);
		}

		sleep(LOOP_DELAY);


		if (o.U.p1 + o.U.p2 + o.U.p3 > 0) {
			units_get++;
			if (units_get > SKIP_SEND) {
				fprintf(f_log, " send ");
				send_base_mqtt();
				units_get = 0;

			}
		} else {
			printf("zero values\n");
		}
	}
	if (OK != closeConnection(fd))
		exitFailure("Power meter connection closing error.");

	send_base_mqtt();

// =============================== power detect logic ================================


	close(fd);
	fclose(f_log);
	tcsetattr(fd, TCSANOW, &oldtio);

	mosquitto_destroy(mosq);
	mosquitto_lib_cleanup();
	exit(EXIT_OK);
}


