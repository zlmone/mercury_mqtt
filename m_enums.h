typedef enum {
	OK = 0,
	ILLEGAL_CMD = 1,
	INTERNAL_COUNTER_ERR = 2,
	PERMISSION_DENIED = 3,
	CLOCK_ALREADY_CORRECTED = 4,
	CHANNEL_ISNT_OPEN = 5,
	WRONG_RESULT_SIZE = 256,
	WRONG_CRC = 257,
	CHECK_CHANNEL_TIME_OUT = 258
} ResultCode;

typedef enum {
	EXIT_OK = 0,
	EXIT_FAIL = 1
} ExitCode;

typedef enum { 			// How much energy consumed: 
	PP_RESET = 0,		// from reset
	PP_YTD = 1,		// this year
	PP_LAST_YEAR = 2,	// last year
	PP_MONTH = 3,		// for the month specified
	PP_TODAY = 4,		// today
	PP_YESTERDAY = 5	// yesterday
} PowerPeriod;


