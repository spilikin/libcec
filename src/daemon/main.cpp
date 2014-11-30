#include "../env.h"
#include "../include/cec.h"

#include <cstdio>
#include <fcntl.h>
#include <iostream>
#include <fstream>
#include <string>
#include <sstream>
#include <signal.h>
#include <stdio.h>
#include <sys/types.h> 
#include <sys/socket.h>
#include <netinet/in.h>
#include "../lib/platform/os.h"
#include "../lib/implementations/CECCommandHandler.h"
#include "../lib/platform/util/StdString.h"
#include "../lib/platform/threads/threads.h"
#include <thread>
#include <errno.h>
#include <string.h>
#include <ctime>

using namespace CEC;
using namespace std;
using namespace PLATFORM;

#define CEC_CONFIG_VERSION CEC_CLIENT_VERSION_CURRENT;

#define CEC_DAEMON_DEFAULT_PORT 5062
#define CEC_DAEMON_DEFAULT_LOGFILE "/var/local/log/appliance/cec.log"

#include "../../include/cecloader.h"

const cec_logical_address CEC_DEVICE_TV = (cec_logical_address)0;
const cec_logical_address CEC_DEVICE_ADAPTER = (cec_logical_address)1;

ICECCallbacks        g_callbacks;
libcec_configuration g_config;
int                  g_cecLogLevel(-1);
int                  g_cecDefaultLogLevel(CEC_LOG_ALL);
ofstream             g_logOutput;
bool                 g_bShortLog(false);
CStdString           g_strPort;
bool                 g_bSingleCommand(false);
bool                 g_bExit(false);
bool                 g_bHardExit(false);
CMutex               g_outputMutex;
ICECAdapter*         g_parser;
int                  g_socket_server; 
FILE*                g_logfile;
bool                 g_cec_compact_log(true);

void on_signal(int iSignal) {
	printf("signal caught: %d - exiting\n", iSignal);
	g_bExit = true;
}

static CStdString cec_log_level_str(cec_log_level level) {
	CStdString strLevel;
    switch (level)
    {
    case CEC_LOG_ERROR:
      strLevel = "ERROR  ";
      break;
    case CEC_LOG_WARNING:
      strLevel = "WARNING";
      break;
    case CEC_LOG_NOTICE:
      strLevel = "NOTICE ";
      break;
    case CEC_LOG_TRAFFIC:
      strLevel = "TRAFFIC";
      break;
    case CEC_LOG_DEBUG:
      strLevel = "DEBUG  ";
      break;
    default:
      strLevel = "-      ";
  	}

	return strLevel;
}

static void log(cec_log_level level, const char *strFormat, ...) {
	CStdString strLog;

	CStdString str_level = cec_log_level_str(level);
	
	va_list argList;
	va_start(argList, strFormat);
	strLog.FormatV(strFormat, argList);
	va_end(argList);
	
	time_t rawtime;
	tm* timeinfo;
	char buffer [80];

	time(&rawtime);
	timeinfo = localtime(&rawtime);

	strftime(buffer,80,"%Y-%m-%dT%H:%M:%S",timeinfo);	
	
	fprintf(g_logfile, "%s %s %s\n", buffer, str_level.c_str(), strLog.c_str());
	fflush(g_logfile);
}

int onCecLogMessage(void *UNUSED(cbParam), const cec_log_message message)
{
	// if comnactp log is wished, ignore some messages
	if (g_cec_compact_log) {
		if (message.level == CEC_LOG_TRAFFIC) {
			return 0;
		}
		if (strstr(message.message, "POLL:") != NULL) {
			return 0;
		}
	}

	log(message.level, "CEC_LOG: %s", message.message);
	return 0;
}

int onCecKeyPress(void *UNUSED(cbParam), const cec_keypress key)
{
	log(CEC_LOG_DEBUG, "CEC_KEY: %d", key.keycode);
 	return 0;
}

int onCecCommand(void *UNUSED(cbParam), const cec_command command)
{
	log(CEC_LOG_DEBUG, "CEC_COMMAND: %s", g_parser->ToString(command.opcode));
	return 0;
}

int onCecAlert(void *UNUSED(cbParam), const libcec_alert type, const libcec_parameter param)
{
	log(CEC_LOG_DEBUG, "CEC_ALERT: type: %d, parameter: %s", type, param.paramData);
 	return 0;
}


/*
 * Structure with information about a single device connected through the HDMI.
 * The information is collected using the CEC interface.
 */
typedef struct device_info {
	CStdString vendor;
	CStdString power_status;
	bool power;
	bool active;
} cec_device_info;

device_info get_device_status(cec_logical_address cec_device, ICECAdapter* parser) {
	cec_device_info result;

    cec_power_status power = parser->GetDevicePowerStatus(cec_device);
	result.power_status = parser->ToString(power);
	if (power == CEC_POWER_STATUS_ON) {
		result.power = true;
	} else {
		result.power = false;
	}

    bool bActive = parser->IsActiveSource(cec_device);
	result.active = bActive;
	
	return result;
}

/*
 * Starts an object using the JSON notation
 */
void json_start(FILE * stream) {
	fprintf(stream, "{"); 
}

/*
 * Ends an object using the JSON notation
 */
void json_end(FILE * stream) {
	fprintf(stream, "}"); 
}


/*
 * Prints a string using the JSON notation (escaped in double quotas)
 */
void json_value(FILE * stream, string str) {
	fprintf(stream, "\"%s\"", str.c_str()); 
}

/*
 * Prints a boolean using the JSON notation
 */
void json_value(FILE * stream, bool val ) {
	if (val == true) {
		fprintf(stream, "true"); 		
	} else {
		fprintf(stream, "false"); 				
	}
}

/*
 * Prints a key in JSON notation: >>>"key":<<<
 */
void json_key(FILE * stream, string key) {
	fprintf(stream, "\"%s\":", key.c_str()); 
}

/*
 * Prints the second key in an object using the JSON notation: >>>,"key":<<<
 */
void json_next_key(FILE * stream, string key) {
	fprintf(stream, ", \"%s\":", key.c_str()); 
}

/*
 * Prints a device information  using the JSON notation
 */
void json_value(FILE * out, device_info device) {
  json_start(out);
  json_key(out, "active");  
  json_value(out, device.active);  

  json_next_key(out, "power");  
  json_value(out, device.power);  

  json_next_key(out, "power_status");  
  json_value(out, device.power_status);

  json_end(out);
}

/*
 * This function is run in a thread and handles a signle client connection
 */
void handle_client(int socket) {
	log(CEC_LOG_DEBUG, "Client connected");
	string input = "";
	char ch;
	
	FILE* io = fdopen(socket, "r+");
	
	while (!g_bExit) {
		if (read(socket, &ch, 1) < 1) {
			if (errno != 0) {
				log(CEC_LOG_ERROR, "Socket read error %s", strerror(errno));				
			} else {
				log(CEC_LOG_DEBUG, "Socket closed");								
			}
			break;
		}

		if (ch == '\r') continue;

		if (ch == '\n') {
			log(CEC_LOG_DEBUG, "Received command: '%s'", input.c_str());					 
			if (input == "quit") {
				break;
			} else if (input == "status") {
				g_parser->RescanActiveDevices();

				device_info tv = get_device_status(CEC_DEVICE_TV, g_parser);
				device_info adapter = get_device_status(CEC_DEVICE_ADAPTER, g_parser);
				
				// TODO: strange bug - none of the devices are active ;(
				if (!tv.active && tv.power && !adapter.active) {
					tv.active = true;
				}
				
				fprintf(io, "OK ");
				json_start(io);
				json_key(io, "tv");  
				json_value(io, tv);
				json_next_key(io, "adapter");
				json_value(io, adapter);
				json_end(io);  
				fprintf(io, "\n");
			} else if (input == "on") {
				g_parser->PowerOnDevices(CEC_DEVICE_TV);
			 	fprintf(io, "OK\n");
			} else if (input == "as") {
				g_parser->SetActiveSource();
			 	fprintf(io, "OK\n");	  	
			} else if (input == "is") {
				g_parser->SetActiveSource(CEC_DEVICE_TYPE_TV);
				g_parser->SetInactiveView();
				g_parser->RescanActiveDevices();
				fprintf(io, "OK\n");	  	
			} else if (input == "standby") {
				g_parser->StandbyDevices(CEC_DEVICE_TV);	  	
				fprintf(io, "OK\n");
			} else {
				fprintf(io, "ERROR Unknown command: '%s'\n", input.c_str());
			}
			fflush(io);
			input.clear();
		} else {
			input += ch;
		}
	}
	fclose(io);
	close(socket);
	log(CEC_LOG_DEBUG, "Client disconnected");
}

/*
 * Runs a socket server at the specified port. Clients can connect to this server to work with CEC.
 */
void run_socket_server(int portno) {
	// portno = atoi("5062");
     struct sockaddr_in serv_addr, cli_addr;
     
	 g_socket_server = socket(AF_INET, SOCK_STREAM, 0);
	 
	 int flags; 
	 /* Set socket to non-blocking */ 
	 if ((flags = fcntl(g_socket_server, F_GETFL, 0)) < 0) 
	 { 
		 log(CEC_LOG_ERROR, "ERROR opening socket: %s", strerror(errno));
		 exit(1);
	 } 

	 if (fcntl(g_socket_server, F_SETFL, flags | O_NONBLOCK) < 0) 
	 { 
		 log(CEC_LOG_ERROR, "ERROR opening socket: %s", strerror(errno));
		 exit(1);
	 } 

		
	 // set socket option, so the bind wont fail after restart
	 int optval = 1;
	 setsockopt(g_socket_server,SOL_SOCKET,SO_REUSEADDR,&optval,sizeof(int));
	 
     if (g_socket_server < 0) {
		 log(CEC_LOG_ERROR, "ERROR opening socket: %s", strerror(errno));
		 exit(1);
     }
	 
     bzero((char *) &serv_addr, sizeof(serv_addr));
     serv_addr.sin_family = AF_INET;
     serv_addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
     serv_addr.sin_port = htons(portno);
     
	 if (bind(g_socket_server, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0) {
		log(CEC_LOG_ERROR, "ERROR on binding: %s", strerror(errno));
		exit(1);
	 }
     listen(g_socket_server,5);
	 	 
	 log(CEC_LOG_DEBUG, "Server started 127.0.0.1:%d", portno);
	 while (!g_bExit && !g_bHardExit) {
		 socklen_t clilen = sizeof(cli_addr);
	     int client_socket = accept(g_socket_server, (struct sockaddr *) &cli_addr, &clilen);
	     if (client_socket < 0) {
			 usleep(500000);
		 	continue;
		 }

		 thread client_thread(handle_client, client_socket);
		 client_thread.detach();
		 		 
	 }
	 
     close(g_socket_server);	
}

int main (int argc, char *argv[]) {
	
	// open logfile
	g_logfile = fopen(CEC_DAEMON_DEFAULT_LOGFILE, "a");
	if (g_logfile == NULL) {
		fprintf(stderr, "Cannot open logfile %s: %s\n", CEC_DAEMON_DEFAULT_LOGFILE, strerror(errno));
		exit(1);
	} 
	
	log(CEC_LOG_DEBUG, "Starting cec-daemon");	
	
	for (int i=0;i<argc;i++) {
		log(CEC_LOG_DEBUG, "argv[%d] = %s", i, argv[i]);			
	}
	
    if (signal(SIGINT, on_signal) == SIG_ERR)
    {
		log(CEC_LOG_ERROR, "can't register sighandler");
     	return -1;
    }
	
    g_config.Clear();
    g_callbacks.Clear();
    snprintf(g_config.strDeviceName, 13, "ITrygg");
    g_config.clientVersion       = CEC_CONFIG_VERSION;
    g_config.bActivateSource     = 0;
    g_callbacks.CBCecLogMessage  = &onCecLogMessage;
    g_callbacks.CBCecKeyPress    = &onCecKeyPress;
    g_callbacks.CBCecCommand     = &onCecCommand;
    g_callbacks.CBCecAlert       = &onCecAlert;	
    g_config.callbacks           = &g_callbacks;
	
	g_config.deviceTypes.Add(CEC_DEVICE_TYPE_RECORDING_DEVICE);
	
	g_parser = LibCecInitialise(&g_config);
    g_parser->InitVideoStandalone();

    cec_adapter devices[10];
	log(CEC_LOG_DEBUG, "Looking for CEC Adapter");	
    uint8_t iDevicesFound = g_parser->FindAdapters(devices, 10, NULL);
	
	if (iDevicesFound < 1) 
	{
		log(CEC_LOG_ERROR, "CEC Adapter not found");
		UnloadLibCec(g_parser);
		exit(1);
	}

	g_strPort = devices[0].comm;
	
    if (!g_parser->Open(g_strPort.c_str()))
    {
      log(CEC_LOG_ERROR, "Unable to open the CEC Adapter on port: %s", g_strPort.c_str());
      UnloadLibCec(g_parser);
      return 1;
    }

	log(CEC_LOG_DEBUG, "Opened CEC Adapter: %s", g_strPort.c_str());
	run_socket_server(CEC_DAEMON_DEFAULT_PORT);
	
    g_parser->Close();
    UnloadLibCec(g_parser);
	fclose(g_logfile);
	return 0;
}
