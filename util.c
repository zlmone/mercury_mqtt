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
#include <time.h>
#include <netdb.h>
#include <arpa/inet.h>
#include <sys/un.h>
#include <stdio.h>
#include<mosquitto.h>


void getDateTimeStr(char *str, int length, time_t time) {
        struct tm *ti = localtime(&time);
        snprintf(str, length, "%4d-%02d-%02d %02d:%02d:%02d", ti->tm_year+1900, ti->tm_mon+1, ti->tm_mday, ti->tm_hour, ti->tm_min, ti->tm_sec);
}


void writeValue(const char* filename, const float value) {
	FILE *f;
	f = fopen(filename, "w");
	fprintf(f, "%.2f", value);
	fclose(f);
}

float readValue(const char* filename) {
	char read_el[16];
	float val = 0;
	FILE *fp=fopen(filename, "r");
	if(fp == NULL){
		return 0;
	}
	fgets(read_el, 16, fp);
	if (read_el != NULL) {
        	sscanf(read_el, "%f", &val);
	}
	fclose(fp);
	return val;
}


int ping(const char * addr, u_short port) {
	struct sockaddr_in address;
	short int sock = -1;
	fd_set fdset;
	struct timeval tv;
	address.sin_family = AF_INET;
	address.sin_addr.s_addr = inet_addr(addr);
	address.sin_port = htons(port);
	sock = socket(AF_INET, SOCK_STREAM, 0);
	fcntl(sock, F_SETFL, O_NONBLOCK);
	connect(sock, (struct sockaddr *)&address, sizeof(address));
	FD_ZERO(&fdset);
	FD_SET(sock, &fdset);
	tv.tv_sec = 0;
	tv.tv_usec = 10000;
	int res = 0;
	if (select(sock + 1, NULL, &fdset, NULL, &tv) == 1) {
	        int so_error;
	        socklen_t len = sizeof so_error;
	        getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_error, &len);
	        if (so_error == 0) {
			res = 1;
	        } else {
			res = 1;
		}
	}
	close(sock);
	return res;
}

float getFloatExec(char *cmd) {
        FILE *fp;
        char path[1035];
        fp = popen(cmd, "r");
        if (fp == NULL) {
                return 0;
        }
        fgets(path, sizeof(path), fp);
        float val;
        sscanf(path, "%f", &val);
        pclose(fp);
        return val;
}

