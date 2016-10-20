// butt - broadcast using this tool
//
// Copyright 2007-2008 by Daniel Noethen.
//
// This program is free software; you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation; either version 2, or (at your option)
// any later version.
//
// This program is distributed in the hope that it will be useful,
// but WITHOUT ANY WARRANTY; without even the implied warranty of
// MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
// GNU General Public License for more details.
//


#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Daniel Litzbach
#include <iostream>
#include <fstream>

#include <lame/lame.h>
#include <signal.h>
#include <limits.h>
#include <pthread.h>

#ifdef _WIN32
 #include <time.h>
 #define IDI_ICON 101 
#endif

#if ! defined(__APPLE__) && !defined(WIN32)  
 #include <FL/Fl_File_Icon.H>
#endif

#include "config.h"

#include "cfg.h"
#include "butt.h"
#include "port_audio.h"
#include "lame_encode.h"
#include "opus_encode.h"
#include "flac_encode.h"
#include "aac_encode.h"
#include "shoutcast.h"
#include "parseconfig.h"
#include "vu_meter.h"
#include "util.h"
#include "flgui.h"
#include "fl_funcs.h"
#include "fl_timer_funcs.h"


bool record;
bool recording;
bool connected;
bool streaming;
bool disconnect;
bool try_connect;
bool song_timeout_running;

int stream_socket;
double kbytes_sent;
double kbytes_written;
unsigned int record_start_hour;

// Daniel Litzbach
char commandfile_path[100];
char statusfile_path[100];
std::string last_command;

sec_timer rec_timer;
sec_timer stream_timer;

lame_enc lame_stream;
lame_enc lame_rec;
vorbis_enc vorbis_stream;
vorbis_enc vorbis_rec;
opus_enc opus_stream;
opus_enc opus_rec;
flac_enc flac_rec;
aac_enc aac_stream;
aac_enc aac_rec;

void reset_commandfile()
{
	std::ofstream commandfile_handle;
	commandfile_handle.open(commandfile_path);
	commandfile_handle << "";
	commandfile_handle.close();
}

// Daniel Litzbach
void process_commandfile(void*)
{
	//print_info("Ping",1);
	std::string line;
	std::ifstream commandfile_handle(commandfile_path);
	if(commandfile_handle.is_open())
	{
		getline(commandfile_handle,line);
		commandfile_handle.close();
		
		if(line != last_command) // Neuer Befehl
		{
			if(line == "conn")
			{
				//cout << "connected" << endl;
				button_connect_cb();
			}
			else if(line == "disconn")
				{
				//cout << "disconnected" << endl;
				button_disconnect_cb();
			}
			last_command=line;
			reset_commandfile();
		}
		
	} else {
		print_info("Commandfile nicht lesbar",1);
	}
	commandfile_handle.close();
	
	Fl::repeat_timeout(0.2, process_commandfile);
}

int main(int argc, char *argv[])
{

    char *p;
    char lcd_buf[33];
    char info_buf[256];
    
    // Daniel Litzbach
    strcpy(commandfile_path,getenv("HOME"));
    strcat(commandfile_path,"/.butt_command.dat");
    
    // Daniel Litzbach: call onDisconnect (init)
    strcpy(statusfile_path,getenv("HOME"));
    strcat(statusfile_path,"/.butt_status.dat");
    std::ofstream statusfile_handle;
    statusfile_handle.open(statusfile_path);
    statusfile_handle << "disconn";
    statusfile_handle.close();

#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); //ignore the SIGPIPE signal.
        //(in case the server closes the connection unexpected)
#endif
    
#if ! defined(__APPLE__) && !defined(WIN32)  
    Fl_File_Icon::load_system_icons();
#endif

    fl_g = new flgui(); 
    fl_g->window_main->show();

#ifdef _WIN32 
    fl_g->window_main->icon((char *)LoadIcon(fl_display, MAKEINTRESOURCE(IDI_ICON)));
    // The fltk icon code above only loads the default icon. 
    // Here, once the window is shown, we can assign 
    // additional icons, just to make things a bit nicer. 
    HANDLE bigicon = LoadImage(GetModuleHandle(0), MAKEINTRESOURCE(IDI_ICON), IMAGE_ICON, 32, 32, 0); 
    SendMessage(fl_xid(fl_g->window_main), WM_SETICON, ICON_BIG, (LPARAM)bigicon); 
    HANDLE smallicon = LoadImage(GetModuleHandle(0), MAKEINTRESOURCE(IDI_ICON), IMAGE_ICON, 16, 16, 0); 
    SendMessage(fl_xid(fl_g->window_main), WM_SETICON, ICON_SMALL, (LPARAM)smallicon); 
#endif 


    snprintf(info_buf, sizeof(info_buf), "Starting %s\nWritten by Daniel Nöthen\n"
    	"Supercharged by Daniel Litzbach / SV98 Fanradio\n", PACKAGE_STRING);
    print_info(info_buf, 0);

#ifdef _WIN32
    if((argc > 2) && (!strcmp(argv[1], "-c")))
    {
        cfg_path = (char*)malloc((strlen(argv[2])+1) * sizeof(char));
        strcpy(cfg_path, argv[2]);
    }
    else if((p = getenv("APPDATA")) == NULL)
    {
        //If there is no "%APPDATA% we are probably in none-NT Windows
        //So we save the config file to the programm directory
        cfg_path = (char*)malloc(strlen(CONFIG_FILE)+1);
        strcpy(cfg_path, CONFIG_FILE);
    }
    else
    {
        cfg_path = (char*)malloc((PATH_MAX+strlen(CONFIG_FILE)+1) * sizeof(char));
        snprintf(cfg_path, PATH_MAX+strlen(CONFIG_FILE), "%s\\%s", p, CONFIG_FILE);  
    }
#else //UNIX
    p = getenv("HOME");
    if((argc > 2) && (!strcmp(argv[1], "-c")))
    {
        cfg_path = (char*)malloc((strlen(argv[2])+1) * sizeof(char));
        strcpy(cfg_path, argv[2]);
    }
    else if(p == NULL)
    {
        ALERT("No home-directory found");
        return 1;
    }
    else
    {
        cfg_path = (char*)malloc((PATH_MAX+strlen(CONFIG_FILE)+1) * sizeof(char));
        snprintf(cfg_path, PATH_MAX+strlen(CONFIG_FILE), "%s/%s", p, CONFIG_FILE);  
    }
#endif

    lame_stream.gfp = NULL;
    lame_rec.gfp = NULL;
    flac_rec.encoder = NULL;
    aac_stream.handle = NULL;
    aac_rec.handle = NULL;

    snprintf(info_buf, sizeof(info_buf), "Reading config %s", cfg_path);
    print_info(info_buf, 0);

    if(snd_init() != 0)
    {
        ALERT("PortAudio init failed\nbutt is going to close now");
        return 1;
    }

    if(cfg_set_values(NULL) != 0)        //read config file and initialize config struct
    {
        snprintf(info_buf, sizeof(info_buf), "Could not find config %s", cfg_path);
        print_info(info_buf, 1);

        if(cfg_create_default())
        {
            fl_alert("Could not create config %s\nbutt is going to close now", cfg_path);
            return 1;
        }
        sprintf(info_buf, "butt created a default config at\n%s\n",
                cfg_path );

        print_info(info_buf, 0);
        cfg_set_values(NULL);
    }

    init_main_gui_and_audio();

    snd_open_stream();

    vu_init();

    Fl::add_timeout(0.01, &vu_meter_timer);
    Fl::add_timeout(5, &display_rotate_timer);
    Fl::add_timeout(0.2, process_commandfile); // Daniel Litzbach

    strcpy(lcd_buf, "idle");
    PRINT_LCD(lcd_buf, strlen(lcd_buf), 0, 1);

	if(cfg.main.connect_at_startup)
		button_connect_cb();

    snprintf(info_buf, sizeof(info_buf),
            "butt %s started successfully", VERSION);
    print_info(info_buf, 0);

    GUI_LOOP();

    return 0;
}

