#include <unistd.h>
#include <sys/wait.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h> //termios, TCSANOW, ECHO, ICANON
#include <string.h>
#include <stdbool.h>
#include <errno.h>
#include <sys/stat.h>
#include <dirent.h> 
#define READ_END 0
#define WRITE_END 1
const char *sysname = "shellfyre";
char *hist[10];
int counter=0;
enum return_codes
{
	SUCCESS = 0,
	EXIT = 1,
	UNKNOWN = 2,
};

struct command_t
{
	char *name;
	bool background;
	bool auto_complete;
	int arg_count;
	char **args;
	char *redirects[3];		// in/out redirection
	struct command_t *next; // for piping
};

/**
 * Prints a command struct
 * @param struct command_t *
 */
void print_command(struct command_t *command)
{
	int i = 0;
	printf("Command: <%s>\n", command->name);
	printf("\tIs Background: %s\n", command->background ? "yes" : "no");
	printf("\tNeeds Auto-complete: %s\n", command->auto_complete ? "yes" : "no");
	printf("\tRedirects:\n");
	for (i = 0; i < 3; i++)
		printf("\t\t%d: %s\n", i, command->redirects[i] ? command->redirects[i] : "N/A");
	printf("\tArguments (%d):\n", command->arg_count);
	for (i = 0; i < command->arg_count; ++i)
		printf("\t\tArg %d: %s\n", i, command->args[i]);
	if (command->next)
	{
		printf("\tPiped to:\n");
		print_command(command->next);
	}
}

/**
 * Release allocated memory of a command
 * @param  command [description]
 * @return         [description]
 */
int free_command(struct command_t *command)
{
	if (command->arg_count)
	{
		for (int i = 0; i < command->arg_count; ++i)
			free(command->args[i]);
		free(command->args);
	}
	for (int i = 0; i < 3; ++i)
		if (command->redirects[i])
			free(command->redirects[i]);
	if (command->next)
	{
		free_command(command->next);
		command->next = NULL;
	}
	free(command->name);
	free(command);
	return 0;
}

/**
 * Show the command prompt
 * @return [description]
 */
int show_prompt()
{
	char cwd[1024], hostname[1024];
	gethostname(hostname, sizeof(hostname));
	getcwd(cwd, sizeof(cwd));
	printf("%s@%s:%s %s$ ", getenv("USER"), hostname, cwd, sysname);
	return 0;
}

/**
 * Parse a command string into a command struct
 * @param  buf     [description]
 * @param  command [description]
 * @return         0
 */
int parse_command(char *buf, struct command_t *command)
{
	const char *splitters = " \t"; // split at whitespace
	int index, len;
	len = strlen(buf);
	while (len > 0 && strchr(splitters, buf[0]) != NULL) // trim left whitespace
	{
		buf++;
		len--;
	}
	while (len > 0 && strchr(splitters, buf[len - 1]) != NULL)
		buf[--len] = 0; // trim right whitespace

	if (len > 0 && buf[len - 1] == '?') // auto-complete
		command->auto_complete = true;
	if (len > 0 && buf[len - 1] == '&') // background
		command->background = true;

	char *pch = strtok(buf, splitters);
	command->name = (char *)malloc(strlen(pch) + 1);
	if (pch == NULL)
		command->name[0] = 0;
	else
		strcpy(command->name, pch);

	command->args = (char **)malloc(sizeof(char *));

	int redirect_index;
	int arg_index = 0;
	char temp_buf[1024], *arg;

	while (1)
	{
		// tokenize input on splitters
		pch = strtok(NULL, splitters);
		if (!pch)
			break;
		arg = temp_buf;
		strcpy(arg, pch);
		len = strlen(arg);

		if (len == 0)
			continue;										 // empty arg, go for next
		while (len > 0 && strchr(splitters, arg[0]) != NULL) // trim left whitespace
		{
			arg++;
			len--;
		}
		while (len > 0 && strchr(splitters, arg[len - 1]) != NULL)
			arg[--len] = 0; // trim right whitespace
		if (len == 0)
			continue; // empty arg, go for next

		// piping to another command
		if (strcmp(arg, "|") == 0)
		{
			struct command_t *c = malloc(sizeof(struct command_t));
			int l = strlen(pch);
			pch[l] = splitters[0]; // restore strtok termination
			index = 1;
			while (pch[index] == ' ' || pch[index] == '\t')
				index++; // skip whitespaces

			parse_command(pch + index, c);
			pch[l] = 0; // put back strtok termination
			command->next = c;
			continue;
		}

		// background process
		if (strcmp(arg, "&") == 0)
			continue; // handled before

		// handle input redirection
		redirect_index = -1;
		if (arg[0] == '<')
			redirect_index = 0;
		if (arg[0] == '>')
		{
			if (len > 1 && arg[1] == '>')
			{
				redirect_index = 2;
				arg++;
				len--;
			}
			else
				redirect_index = 1;
		}
		if (redirect_index != -1)
		{
			command->redirects[redirect_index] = malloc(len);
			strcpy(command->redirects[redirect_index], arg + 1);
			continue;
		}

		// normal arguments
		if (len > 2 && ((arg[0] == '"' && arg[len - 1] == '"') || (arg[0] == '\'' && arg[len - 1] == '\''))) // quote wrapped arg
		{
			arg[--len] = 0;
			arg++;
		}
		command->args = (char **)realloc(command->args, sizeof(char *) * (arg_index + 1));
		command->args[arg_index] = (char *)malloc(len + 1);
		strcpy(command->args[arg_index++], arg);
	}
	command->arg_count = arg_index;
	return 0;
}

void prompt_backspace()
{
	putchar(8);	  // go back 1
	putchar(' '); // write empty over
	putchar(8);	  // go back 1 again
}

/**
 * Prompt a command from the user
 * @param  buf      [description]
 * @param  buf_size [description]
 * @return          [description]
 */
int prompt(struct command_t *command)
{
	int index = 0;
	char c;
	char buf[4096];
	static char oldbuf[4096];

	// tcgetattr gets the parameters of the current terminal
	// STDIN_FILENO will tell tcgetattr that it should write the settings
	// of stdin to oldt
	static struct termios backup_termios, new_termios;
	tcgetattr(STDIN_FILENO, &backup_termios);
	new_termios = backup_termios;
	// ICANON normally takes care that one line at a time will be processed
	// that means it will return if it sees a "\n" or an EOF or an EOL
	new_termios.c_lflag &= ~(ICANON | ECHO); // Also disable automatic echo. We manually echo each char.
	// Those new settings will be set to STDIN
	// TCSANOW tells tcsetattr to change attributes immediately.
	tcsetattr(STDIN_FILENO, TCSANOW, &new_termios);

	// FIXME: backspace is applied before printing chars
	show_prompt();
	int multicode_state = 0;
	buf[0] = 0;

	while (1)
	{
		c = getchar();
		// printf("Keycode: %u\n", c); // DEBUG: uncomment for debugging

		if (c == 9) // handle tab
		{
			buf[index++] = '?'; // autocomplete
			break;
		}

		if (c == 127) // handle backspace
		{
			if (index > 0)
			{
				prompt_backspace();
				index--;
			}
			continue;
		}
		if (c == 27 && multicode_state == 0) // handle multi-code keys
		{
			multicode_state = 1;
			continue;
		}
		if (c == 91 && multicode_state == 1)
		{
			multicode_state = 2;
			continue;
		}
		if (c == 65 && multicode_state == 2) // up arrow
		{
			int i;
			while (index > 0)
			{
				prompt_backspace();
				index--;
			}
			for (i = 0; oldbuf[i]; ++i)
			{
				putchar(oldbuf[i]);
				buf[i] = oldbuf[i];
			}
			index = i;
			continue;
		}
		else
			multicode_state = 0;

		putchar(c); // echo the character
		buf[index++] = c;
		if (index >= sizeof(buf) - 1)
			break;
		if (c == '\n') // enter key
			break;
		if (c == 4) // Ctrl+D
			return EXIT;
	}
	if (index > 0 && buf[index - 1] == '\n') // trim newline from the end
		index--;
	buf[index++] = 0; // null terminate string

	strcpy(oldbuf, buf);

	parse_command(buf, command);

	// print_command(command); // DEBUG: uncomment for debugging

	// restore the old settings
	tcsetattr(STDIN_FILENO, TCSANOW, &backup_termios);
	return SUCCESS;
}

int process_command(struct command_t *command);
int runTake(char** args);
int chronometer(char** args);
int filesearch(char** args);
int runcdh();
int runJoker();
int doKernel(char** args);
int calculator(char** args);
int main()
{
	
	for (int i = 0; i < 10; i++)
        hist[i] = NULL;
	while (1)
	{
		struct command_t *command = malloc(sizeof(struct command_t));
		memset(command, 0, sizeof(struct command_t)); // set all bytes to 0
		
		int code;
		code = prompt(command);
		if (strcmp(command->name, "cd") == 0)
		{
			if (command->arg_count > 0)
			{
				
				
			}
		}
		if (code == EXIT)
			break;

		code = process_command(command);
		if (code == EXIT)
			break;

		free_command(command);
	}

	printf("\n");
	return 0;
}

int process_command(struct command_t *command)
{	
	
	int r;
	if (strcmp(command->name, "") == 0)
		return SUCCESS;

	if (strcmp(command->name, "exit") == 0)
		return EXIT;

	if (strcmp(command->name, "cd") == 0)
	{
		if (command->arg_count > 0)
		{
			char cwd[100];
			getcwd(cwd, sizeof(cwd));
			counter = counter%10;
			hist[counter] = strdup(cwd);
			counter++;
			r = chdir(command->args[0]);
			if (r == -1)
				printf("-%s: %s: %s\n", sysname, command->name, strerror(errno));
			return SUCCESS;
		}
	}

	// TODO: Implement your custom commands here
	
	if (strcmp(command->name, "filesearch") == 0)
	{
		filesearch(command->args);
		return SUCCESS;
	}
	if (strcmp(command->name, "cdh") == 0)
	{
		runcdh();
		return SUCCESS;
	}
	if (strcmp(command->name, "take") == 0)
	{
		runTake(command->args);
		return SUCCESS;
	}
	if (strcmp(command->name, "joker") == 0)
	{
		runJoker();
		return SUCCESS;
	}
	if (strcmp(command->name, "chronometer") == 0)
	{
		chronometer(command->args);
		return SUCCESS;
	}
	if (strcmp(command->name, "calculate") == 0)
	{
		calculator(command->args);
		return SUCCESS;
	}
	if (strcmp(command->name, "pstraverse") == 0)
	{
		doKernel(command->args);
		return SUCCESS;
	}
	pid_t pid = fork();

	if (pid == 0) // child
	{
		// increase args size by 2
		command->args = (char **)realloc(
			command->args, sizeof(char *) * (command->arg_count += 2));

		// shift everything forward by 1
		for (int i = command->arg_count - 2; i > 0; --i)
			command->args[i] = command->args[i - 1];

		// set args[0] as a copy of name
		command->args[0] = strdup(command->name);
		// set args[arg_count-1] (last) to NULL
		command->args[command->arg_count - 1] = NULL;

		/// TODO: do your own exec with path resolving using execv()
		char path[100]="/bin/";
		strcat(path,command->args[0]);
		memcpy(command->args[0],path,strlen(path)+1);
		execv(path,command->args);		
		exit(0);
	}
	else
	{
		/// TODO: Wait for child to finish if command is not running in background
		if (!command->background)
			wait(0);

		return SUCCESS;
	}

	printf("-%s: %s: command not found\n", sysname, command->name);
	return UNKNOWN;
}


int runTake(char** args){

	char * token = strtok(args[0], "/");
   
        while( token != NULL ) {
              printf( " %s\n", token ); //printing each token
              mkdir(token, S_IRWXU);
              chdir(token);
              token = strtok(NULL, "/");
        }
	return 0;
}
int chronometer(char** args){

	int seconds = atoi(args[0]);
	printf("\nNow It Starts\n");
	while(seconds != 0) {
		printf("second : %d\n",seconds);
		seconds--;
		sleep(1);
	}
	
	printf("\n Time is up !!!\n");
}

int filesearch(char** args){

	DIR *d;
	struct dirent *dir;
	d = opendir(".");
	char *openFile = "cd";
	
	if (d) {
	  while ((dir = readdir(d)) != NULL) {
	    if(args[2] != NULL){
	    	if(strstr(dir->d_name, args[2])){

			DIR *c;
			c = opendir(dir->d_name);
			pid_t pid;
			pid=fork();

			if(pid==0){
				char *cmd[] = {"/bin/xdg-open", dir->d_name, 0};        		
				execv(cmd[0], cmd);
				exit(0);
			}else{
				wait(0);
			}
			if(c){
			  	chdir(dir->d_name);
			  	filesearch(args);
			  	closedir(c);
			}
		    	
		 }
	    }else if(args[1] != NULL){
	    	if(strstr(dir->d_name, args[1])){
		    	if(strcmp(args[0],"-r") == 0){
			  DIR *c;
			  c = opendir(dir->d_name);
			  if(c){
			  	chdir(dir->d_name);
			  	filesearch(args);
			  	closedir(c);
			  }
		    	}else{
		    		pid_t pid;
				pid=fork();

				if(pid==0){
					char *cmd[] = {"/bin/xdg-open", dir->d_name, 0};        		
					execv(cmd[0], cmd);
					exit(0);
				}else{
					wait(0);
				}
					
		    	}
		  	
		     
		   }
	    }else{
	    	if(strstr(dir->d_name, args[0])){
	    		printf("%s\n", dir->d_name);
	    	}
	    }
	    
	  }
	    closedir(d);
	}
	  
      return(0);

}

int runcdh() {

	char letter = 'k';
	for(int t=9;t>=0;t--){
	        letter--;
		if(hist[t] != NULL){
			printf("%c  %d) %s \n" ,letter, t,hist[t]);
		}
	}
	if(hist[0] == NULL) {
		printf("There is no history\n");
		return 0;
	}
	int fd[2];
	pid_t pid;
	if (pipe(fd) == -1) {
		fprintf(stderr, "Pipe failed");
		return 1;
	}
	pid = fork();
	if (pid < 0) {
		fprintf(stderr, "Fork failed");
		return 1;
	}
	if (pid == 0) {
		printf("please provide a letter\n");
	        char let;
    		scanf("%c", &let);
    		
		close(fd[READ_END]);
		write(fd[WRITE_END], &let, sizeof(char));
		close(fd[WRITE_END]);
		exit(0);
		
		
	}else {
		wait(0);
		close(fd[WRITE_END]);
		char let;
		int res;
		read(fd[READ_END], &let, sizeof(char));
		res = let;
		if(97<=let && let<107){
			res = let-97;
			chdir(hist[res]);
		}else if(48<=let && let<58){
			res = let-48;
			chdir(hist[res]);
		}else{
			printf("Invalid index");
		}
		close(fd[READ_END]);
		kill(pid,SIGKILL);
	}
	return 0;
	
}
int runJoker() {
	FILE * fp;
   
	/* open the file for writing*/
   	fp = fopen("cronFile.txt","w");
	fprintf(fp,"%s","*/15 * * * * XDG_RUNTIME_DIR=/run/user/$(id -u) /usr/bin/notify-send \"$(/bin/curl https://icanhazdadjoke.com/)\"\n ");
   	fclose (fp);
   	//executes the crontab.
	pid_t pid=fork();
	if(pid==0){	
		char *cronArgs[] = { "/usr/bin/crontab", "cronFile.txt", 0 };
   		execv(cronArgs[0], cronArgs);	
	}else{
		wait(&pid);
	}
	return 0;
}
int calculator(char** args){

   	int num1 = atoi(args[0]);
   	int num2 = atoi(args[2]);
        if(strcmp(args[1],"+") ==0){
        	printf("The result is: %d\n",num1+num2);
        }else if(strcmp(args[1],"-") ==0){
        	printf("The result is: %d\n",num1-num2);
        }else if(strcmp(args[1],"*") ==0){
        	printf("The result is: %d\n",num1*num2);
        }else if(strcmp(args[1],"/") ==0){
        	printf("The result is: %d\n",num1/num2);
        }else if(strcmp(args[1],"%") ==0){
        	printf("The result is: %d\n",num1%num2);
        }else{
        	printf("The operator is not valid!!!");
        }
	return 0;
	
}

int doKernel(char** args){
	return 0;
}
