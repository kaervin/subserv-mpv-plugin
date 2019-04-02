// Build with:
// gcc -o ~/.config/mpv/scripts/subserv.so subserv.c -I . -shared -fPIC

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <sys/socket.h>
#include <stdlib.h>
#include <netinet/in.h>
#include <sys/types.h>
#include <netdb.h>

#include <fcntl.h>
#include <unistd.h>

#include <pthread.h>
#include <client.h>

#define MAX(a,b) (((a)>(b))?(a):(b))
#define PORT 8080

// modifies the buffer, so watch out
// return the number of words in the line and stores pointers to the individual words in word_pointers
int split_line_into_words(char *line, char **word_pointers, int max_words) {
	int num_words = 0;
	
	int char_counter = 0;
	char *current_char = line;
	
	// consume everything until a word starts, or until there is some end signifier
	while(*current_char != '\n' || *current_char != '\0') {
		if(*current_char == ' ' || *current_char == '\t') {
			char_counter++;
			current_char = &line[char_counter];
		} else {
			word_pointers[num_words] = current_char;
			num_words++;
			char_counter++;
			current_char = &line[char_counter];
			break;
		}
	}
	
	if (max_words == 1) {
		return num_words;
	}
	
	// now break up the following words
	while(*current_char != '\n' || *current_char != '\0') {
		if (*current_char == ' ') {
			*current_char = '\0';
			word_pointers[num_words] = &line[char_counter+1];
			num_words++;
			if (num_words >= max_words) {
				return num_words;
			}
			
		}
		char_counter++;
		current_char = &line[char_counter];
	}
	return num_words;
};


const char *reply_before_len =
"HTTP/1.1 200 OK\n"
"Content-Type: text/html\n"
"Content-Length: ";


const char *reply_after_len =
"\n"
"Accept-Ranges: bytes\n"
"Connection: close\n"
"\n";

#define STRINGIFY(x) #x

const char * subs_html =
#include "subserv.html"
;

typedef struct Sub_Line {
    int start;
    int length;
} Sub_Line;

// Just use 5 megs, should be enough
int sub_max = 5 << 20;
int sub_next = 0;
char sub_storage[5 << 20];

int lines_next = 0;
int lines_max = 5 << 20;
Sub_Line lines[5 << 20];

// and 6 megs for the out buffer should be enough too
char payload_buffer[6 << 20];
char out_buffer[6 << 20];

/*
\b  Backspace (ascii code 08)
\f  Form feed (ascii code 0C)
\n  New line
\r  Carriage return
\t  Tab
\"  Double quote
\\  Backslash character
*/

void json_escape_sub_and_store(char * in_string) {

    int stor = sub_next;
    int i = 0;
    while (in_string[i] != '\0') {
        char cur = in_string[i];
    
        if (stor + 1 >= lines_max) {
            // if we are out of storage, just reset the stack and start from the beginning
            sub_next = 0;
            lines_next = 0;
            return json_escape_sub_and_store(in_string);
        }
        
        switch(cur) {
        case '\b':
            sub_storage[stor] = '\\';
            stor++;
            sub_storage[stor] = 'b';
            stor++;
            i++;
            break;
        case '\f':
            sub_storage[stor] = '\\';
            stor++;
            sub_storage[stor] = 'f';
            stor++;
            i++;
            break;
        case '\n':
            sub_storage[stor] = '\\';
            stor++;
            sub_storage[stor] = 'n';
            stor++;
            i++;
            break;
        case '\r':
            sub_storage[stor] = '\\';
            stor++;
            sub_storage[stor] = 'r';
            stor++;
            i++;
            break;
        case '\t':
            sub_storage[stor] = '\\';
            stor++;
            sub_storage[stor] = 't';
            stor++;
            i++;
            break;
        case '"':
            sub_storage[stor] = '\\';
            stor++;
            sub_storage[stor] = '"';
            stor++;
            i++;
            break;
        case '\\':
            sub_storage[stor] = '\\';
            stor++;
            sub_storage[stor] = '\\';
            stor++;
            i++;
            break;
        default:
            sub_storage[stor] = cur;
            stor++;
            i++;
        }
    }
    Sub_Line new_line;
    new_line.start = sub_next;
    new_line.length = stor - sub_next;

    sub_next = stor;
    sub_storage[sub_next] = '\0';
    sub_next++;

    if (lines_next >= lines_max) {
        lines_next = 0;
    }
    lines[lines_next] = new_line;
    lines_next++;
}

int subs_html_length;

void send_subs(int client_socket) {
        
        char read_buf[1024];

        int reccnt = recv(client_socket, read_buf, 1024, 0);
        if (reccnt == 0) {
            return;
        }
        printf("read_buf %s\n\n", read_buf);
        
        char * words[3];
        int num_words = split_line_into_words(read_buf, words, 3);
        
        if (num_words != 3)
            return;
            
        if (strcmp("GET", words[0]) != 0)
            return;
        
        if (strncmp("/subs", words[1], 5) == 0) {
        
            int line_nr = 0;
        
            if (words[1][5] == '/') {
                line_nr = atoi(&words[1][6]);
            }
        
            line_nr = MAX(line_nr, 0);
                      
            int pl = snprintf(payload_buffer, 6 << 20, "{\"lines_next\": %i, \"subs\" : [", lines_next);
            int i = line_nr;
            for (; i < lines_next-1; i++) {
                Sub_Line i_line = lines[i];
                pl += snprintf(payload_buffer+pl, (6 << 20)-pl, "\"%s\",", sub_storage+i_line.start);
            }
            if (i == lines_next-1) {
                Sub_Line i_line = lines[lines_next-1];
                pl += snprintf(payload_buffer+pl, (6 << 20)-pl, "\"%s\"", sub_storage+i_line.start);
            }
            pl += snprintf(payload_buffer+pl, (6 << 20)-pl, "]}");
            int n = snprintf(out_buffer, 6 << 20, "%s%i%s%s", reply_before_len, pl, reply_after_len, payload_buffer);

            n = send(client_socket, out_buffer, n, 0);
        } else {
            int n = snprintf(out_buffer, 6 << 20, "%s%i%s%s", reply_before_len, subs_html_length, reply_after_len, subs_html);
            n = send(client_socket, out_buffer, n, 0);
        
        }
}

void *server_start(void *arg) {

    subs_html_length = strlen(subs_html); 

    int sock_fd, client_socket;
    struct sockaddr_in address;
    int addrlen = sizeof(address);
    
    address.sin_family = AF_INET; 
    address.sin_addr.s_addr = INADDR_ANY; 
    address.sin_port = htons(PORT);
    
    int opt = 1;
    
    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == 0) {
        printf("couldn't open socket\n");
        return 0;
    }
        // Forcefully attaching socket to the port 8080 
    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR | SO_REUSEPORT, &opt, sizeof(opt))) 
    { 
        printf("couldn't open socket\n");
        return 0;
    }
    
    if (bind(sock_fd, (struct sockaddr *)&address, sizeof(address)) < 0) {
        printf("couldn't bind socket\n");
        return 0;
    }
    
    if (listen(sock_fd, 3) < 0) {
        printf("couldn't listen on socket\n");
        return 0;
    }

    while(1) {
        client_socket = accept(sock_fd, (struct sockaddr *)&address, (socklen_t*)&addrlen);
        if (client_socket < 0) { 
            printf("couldn't accept client\n");
            return 0;
        }
        
        send_subs(client_socket);
        close(client_socket);
    }
}

int mpv_open_cplugin(mpv_handle *handle) {
    pthread_t tidp;
    
    int pthread_err = pthread_create(&tidp, NULL, &server_start, NULL);
    if (pthread_err != 0) {
        printf("failed at creating a server-thread\n");
        return 0;
    }
    
    printf("Hello world from C plugin '%s'!\n", mpv_client_name(handle));
    
    int obs_ret = mpv_observe_property(handle, 0, "sub-text", MPV_FORMAT_STRING);
    
    while (1) {
        mpv_event *event = mpv_wait_event(handle, -1);
        printf("Got event: %d\n", event->event_id);
        
        if (event->event_id == MPV_EVENT_SHUTDOWN)
            break;
        
        if (event->event_id == MPV_EVENT_PROPERTY_CHANGE) {
            mpv_event_property* event_prop = (mpv_event_property*)event->data;
            
            if (strcmp(event_prop->name, "sub-text") == 0) {
                if (event_prop->data == NULL)
                    continue;
                
                char *value = *(char **)(event_prop->data);
                json_escape_sub_and_store(value);
            }
        }
    }
    return 0;
}

