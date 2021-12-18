#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <sys/sem.h>
#include <semaphore.h>
#include <pthread.h>
#include <netinet/in.h>

#define	QLEN			5
#define	BUFSIZE			4096

int clients = 0;
int gameopen = 0;
int gamefinished = 0;
int players = 0;

typedef struct Player 
{
	char     name[50];
    int      score;
    int      in_quiz;
    int      socket;
} Player;


typedef struct Quiz 
{
	int number_of_questions;
    char questions[128][128];
    char correct_answers[128];
    char possible_answers[3][128][128];
    char winner[50];
} Quiz;


typedef struct Group {
    char            name[50];
    int             max_size;
    int				quest_num;
    Player          players[50];
    int             admin_sock;
    int             curr_size;
} Group;

void *serve(void *ssock);
void *admin(void *ssock);
void *closed(void *ssock);

void* run_quiz(void* ssock);

Group group;
Quiz theQuiz;

pthread_mutex_t mutex, mutexA;
pthread_mutex_t mutexJoin;
pthread_mutex_t mutexInQuiz;
pthread_cond_t condGroup;
pthread_cond_t condInQuiz;

void get_quiz(Quiz *quiz, char *filename);

// gcc QuizServer.c -Llibs -lsocklib -o QuizServer
// cd "Desktop/TTU/Fall 21/Systems Programming"


int passivesock( char *service, char *protocol, int qlen, int *rport );

fd_set clr;


char			buf[BUFSIZE];
int main( int argc, char *argv[] )
{
	char			*service;
	struct sockaddr_in	fsin;
	socklen_t		alen;
	int			msock;
	int			ssock;
	int			rport = 0;
	FD_ZERO(&clr);
	
	switch (argc) 
	{
		case	1:
			// No args? let the OS choose a port and tell the user
			rport = 1;
			break;
		case	2:
			// User provides a port? then use it
			service = argv[1];
			break;
		default:
			fprintf( stderr, "usage: server [port]\n" );
			exit(-1);
	}
	msock = passivesock( service, "tcp", QLEN, &rport );
	if (rport)
	{
		//	Tell the user the selected port
		printf( "server: port %d\n", rport );	
		fflush( stdout );
	}
	for (;;)
	{
		printf("clients: %d, players: %d\n",clients, players);
		fflush(stdout);
		int	ssock;
		alen = sizeof(fsin);
        pthread_t pid;
		ssock = accept( msock, (struct sockaddr *)&fsin, &alen );
		if(clients == 0)
		{
			if (ssock < 0)
			{
				fprintf( stderr, "accept: %s\n", strerror(errno) );
				exit(-1);
			}
			clients++;
			printf( "Admin has arrived.\n" );
			fflush( stdout );
			pthread_create(&pid, NULL, admin, (void *) ssock);
		}
		else
		{
			if (ssock < 0)
			{
				fprintf( stderr, "accept: %s\n", strerror(errno) );
				exit(-1);
			}
			clients++;
			printf( "Client has arrived.\n" );
			pthread_create(&pid, NULL, serve, (void *) ssock);
		}
	}
}

void get_quiz(Quiz *quiz, char *filename)
{
	int qs = 0; //number of questions
	FILE *fquiz = fopen(filename, "r");
	int i;

	while(fgets(buf, BUFSIZE, fquiz)) 
    {
		strcpy(quiz->questions[qs], buf);
        for(i = 0;i<3;i++)
        {
            fgets(buf, BUFSIZE, fquiz);
            printf("read: %s\n", strtok(buf,"\n"));
            fflush(stdout);
            strcpy(quiz->possible_answers[qs][i], buf);
        }
        fgets(buf, BUFSIZE, fquiz);
        fgets(buf, BUFSIZE, fquiz);
        printf("read: %s\n", strtok(buf,"\n"));
        fflush(stdout);
        quiz->correct_answers[qs] = buf[0];
        fgets(buf, BUFSIZE, fquiz);
        qs++;
    }
	quiz->number_of_questions = qs;

}

void *serve(void *ssock)
{
	printf("Client has entered\n");
	int score;
    int cc;
	struct Player p;
	p.score = 0;
	p.in_quiz = 0;
	p.socket = (int)ssock;
    for (;;)
	{

		char msg[BUFSIZE];
		strcpy(msg , " ");
		while(p.in_quiz)
		{
			printf( "this client is in a quiz\n" );
			pthread_cond_wait(&condInQuiz, &mutexInQuiz);
		}
		
		if ( (cc = read( ssock, buf, BUFSIZE )) <= 0 )
			{
				printf( "A client has gone.\n" );
				close(ssock);
				break;
			}
		else
		{
			if (!strncmp("JOIN", buf, 4))
			{
				strtok(buf, "|");
				char *name  = strtok(NULL, "|");
				printf("subtring: [%s]\n", buf);
				if(!(!strcmp(group.name, "")))
				{
					printf("Group exists\n");
					p.name[0] = '\0';
					strcpy(p.name, "");
					strcat(p.name, name);

					printf("Client \"%s\" entered with socket: %d\n",p.name, p.socket);
					if(group.curr_size >= group.max_size)
					{
						printf("FULL");
						send(ssock, "FULL\r\n", strlen("FULL\r\n"), 0);
						continue;
					}
					else if(group.curr_size < group.max_size - 1)
					{
						printf("\"%s\" joined the group\n",p.name);
						group.players[group.curr_size] = p;
						FD_SET(p.socket,&clr);

						strcpy(msg , "Joined the group");
						
						pthread_mutex_lock(&mutexJoin);
						group.curr_size++;
						if (group.curr_size != group.max_size )
							pthread_cond_wait(&condGroup, &mutexJoin);
						else
							pthread_cond_broadcast(&condGroup);
						pthread_mutex_unlock(&mutexJoin);
						
						
					}
					else if(group.curr_size == group.max_size - 1)
					{
						group.curr_size++;
						group.players[group.curr_size] = p;
						FD_SET(p.socket,&clr);

						pthread_t quiz;
						
						if(pthread_create(&quiz, NULL, run_quiz, (void*)group.name) < 0)
						{
							printf(stderr, "ERROR: %s\n", strerror(errno));
							exit(-1);
						}
					}
				}
				else
				{
					printf("Group doesn't exist\n");
				}
			}
			else
			{
				strcpy(msg ,"Unrecognized command");
			}
			write(ssock, msg, strlen(msg));
		}
	}
}
void *admin(void *ssock)
{
	char msg[BUFSIZE];
	int cc;

	for (;;)
	{
		printf("Admin has entered\n");
		fflush( stdout );
		if ( (cc = read( ssock, buf, BUFSIZE )) <= 0 )
			{
				printf( "A client has gone.\n" );
				fflush( stdout );
				close(ssock);
				break;
			}
		else
		{
			if (!strncmp("GROUP", buf, 5))
			{
				int group_size;
  				char *start = &buf[5];
  				char *end = &buf[cc];
 				char *substr = (char *)calloc(1, end - start + 1);
  				memcpy(substr, start, end - start);
				
				char *Lname = strtok(substr, "|");
				char *Lnum = strtok(NULL, "|");

				if (atoi(Lnum) > 0)
				{
					group_size = atoi(Lnum);
					strcpy(msg ,"Joined the group");
				}
				else
					strcpy(msg , "BAD");
				
				group.name[0] = '\0';
				strcpy(group.name, "");
				strcat(group.name, Lname);

				group.max_size = atoi(Lnum);
				group.curr_size = 0;
				group.admin_sock = (int)ssock;

				printf("Created Group: %s\nCurrent players: %d\n%dmaximum players\nadmin socket: %d\n", group.name,group.curr_size,group.max_size, ssock);

				FD_SET(ssock, &clr);
				pthread_mutex_init(&mutex, NULL);
			}
			else
			{
				strcpy(msg ,"Unrecognized command");
			}
			write(ssock, msg, sizeof(msg));
		}
	}
}

void *run_quiz(void *ssock)
{
	int qnum, r, cc;
	printf("STARTING QUIZ\n");
	fflush(stdout);
	int i;
	char msg[BUFSIZE];

	fd_set rfds, wfds, efds;
	fd_set afds, bfds;
	int nfds = 0;
	int admin = group.admin_sock;
	int curr_size = 0;
	int max_size = 0;

	nfds = group.admin_sock + 1;

	FD_ZERO(&afds);
	FD_ZERO(&bfds);
	FD_ZERO(&rfds);
	FD_ZERO(&wfds);
	FD_ZERO(&efds);

	FD_SET(group.admin_sock, &afds);
	FD_CLR(group.admin_sock, &clr);

	curr_size = group.curr_size;
	max_size = group.max_size;

	for(i=1;i<curr_size+1;i++)
	{
		printf("joining player with socket %d\n",group.players[i].socket);
		group.players[i].in_quiz = 1;
		FD_CLR(group.players[i].socket, &clr);
		FD_SET(group.players[i].socket, &afds);
	}

    printf("QUEST_NUM is: %d for group", group.quest_num, index);
    fflush(stdout);

	char *question[] = {"this is question 1","this is question 2","this is question 3"};

    for(qnum = 0; qnum < 3; qnum++)
    {

        for(r = 0; r < group.curr_size; r++)
        {
            write(group.players[r].socket, question[qnum], strlen(question[qnum]));
        }
        memcpy((char *)&rfds, (char *)&afds, sizeof(rfds));

        if (select(nfds, &rfds, (fd_set *)0, (fd_set *)0, NULL) < 0)
        {
            fprintf( stderr, "server select: %s\n", strerror(errno) );
            exit(-1);
        }
        fflush(stdout);
        for(int i = 0; i < nfds; i++)
        {
			char msg[BUFSIZ];
            if (FD_ISSET( i, &rfds))
            {
                if (i + 1 > nfds)
                {
                    nfds = i + 1;
                }

                if ( (cc = read( i, buf, BUFSIZE )) <= 0 )
                {
                    printf( "The client has gone.\n" );
                    (void) close(i);
                    FD_CLR( i, &afds );
                    if (i == group.admin_sock)
                    {
                        FD_ZERO(&afds);
                        FD_ZERO(&bfds);
                        FD_ZERO(&rfds);
                        FD_ZERO(&wfds);
                        FD_ZERO(&efds);
                        pthread_exit(0);
                    }

                    group.curr_size--;
                    if ( nfds == i + 1 ) 
						nfds--;
                }
                else
                {
                    printf("\n buf is %s \n", buf);
                    fflush(stdout);
                    buf[cc - 2] = '\0';
                    if(strncmp(buf, "CANCEL", 6) == 0 && i == group.admin_sock) 
					{

                        char* rest2 = buf;
                        strtok_r(rest2, "|", &rest2); 
                        char *gr_name = (char *)malloc(sizeof(char)*256);
                        gr_name = strtok_r(rest2, "|", &rest2); 
						char end[280];
						strcat(end, "|");
						strcat(end, gr_name);
						strcat(end, "\r\n");
                        for (int k = 0; k < group.curr_size; k++)
                        {
                            send(group.players[k].socket, end, strlen(end), 0);
                            close(group.players[k].socket);
                        }
                    }
					else if(!strncmp(buf, "ANS", 3) && i != group.admin_sock)
					{
						strtok(buf, "|");
						char *ans = strtok(NULL, "|");
						printf("Client at %d answered %s", i, buf);
						fflush(stdout);
						strcpy(msg , "you have answered");
					}
					else
					{
						strcpy(msg , "Invalid answer");
					}
                }
				write(ssock, msg, sizeof(msg));
            }
        }
    }
}

void *closed(void *ssock)
{
	wait(2);
	char msg[BUFSIZE];
	strcpy(msg , "the server is closed");
	int cc;
	write(ssock, msg, sizeof(msg));
	close(ssock);
	pthread_exit(0);
}