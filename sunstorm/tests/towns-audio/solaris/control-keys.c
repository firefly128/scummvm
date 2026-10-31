#define __EXTENSIONS__ 1
#include <SDL.h>
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
static Uint32 lastCheck;
static SDL_Keymod held;
static SDL_Keycode lastKey;
static void key(SDL_Keycode sym,SDL_Scancode scan,SDL_Keymod mod){SDL_Event e;memset(&e,0,sizeof(e));
 SDL_SetModState(mod);held=mod;lastKey=sym;e.type=SDL_KEYDOWN;e.key.state=SDL_PRESSED;e.key.keysym.sym=sym;e.key.keysym.scancode=scan;e.key.keysym.mod=mod;SDL_PushEvent(&e);
 e.type=SDL_KEYUP;e.key.state=SDL_RELEASED;SDL_PushEvent(&e);}
int SDL_PollEvent(SDL_Event *event){static int(*realPoll)(SDL_Event*);Uint32 now;FILE *f;char command[128],arg[100];int x,y,n;
 if(!realPoll)realPoll=(int(*)(SDL_Event*))dlsym(RTLD_NEXT,"SDL_PollEvent");if(!realPoll)return 0;
 now=SDL_GetTicks();
 if(now-lastCheck>=100){lastCheck=now;f=fopen("/var/tmp/tcx-play-control.command","r");if(f){
  command[0]=0;if(fgets(command,sizeof(command),f)){command[strcspn(command,"\r\n")]=0;}fclose(f);unlink("/var/tmp/tcx-play-control.command");
  if(!strcmp(command,"escape"))key(SDLK_ESCAPE,SDL_SCANCODE_ESCAPE,KMOD_NONE);
  else if(!strncmp(command,"key ",4)&&command[4])key((SDL_Keycode)command[4],SDL_GetScancodeFromKey((SDL_Keycode)command[4]),KMOD_NONE);
  else if(sscanf(command,"queue-ms %d",&x)==1&&x>=50&&x<=1000){char value[16];sprintf(value,"%d",x);setenv("SCUMM_AUDIO_QUEUE_MS",value,1);}
  else if(!strcmp(command,"enter"))key(SDLK_RETURN,SDL_SCANCODE_RETURN,KMOD_NONE);
  else if(!strcmp(command,"debugger"))key(SDLK_d,SDL_SCANCODE_D,(SDL_Keymod)(KMOD_CTRL|KMOD_ALT));
  else if(!strcmp(command,"menu"))key(SDLK_F5,SDL_SCANCODE_F5,KMOD_CTRL);
  else if(!strcmp(command,"save"))key(SDLK_F5,SDL_SCANCODE_F5,KMOD_NONE);
  else if(!strcmp(command,"quit"))key(SDLK_q,SDL_SCANCODE_Q,KMOD_CTRL);
  else if(!strncmp(command,"text ",5)){const char *p=command+5;while(*p){SDL_Event e;memset(&e,0,sizeof(e));e.type=SDL_TEXTINPUT;e.text.text[0]=*p++;SDL_PushEvent(&e);}}
  else if(sscanf(command,"move %d %d",&x,&y)==2){SDL_Event e;memset(&e,0,sizeof(e));e.type=SDL_MOUSEMOTION;e.motion.x=x;e.motion.y=y;SDL_PushEvent(&e);}
  else if(sscanf(command,"click %d %d",&x,&y)==2){SDL_Event e;memset(&e,0,sizeof(e));e.type=SDL_MOUSEMOTION;e.motion.x=x;e.motion.y=y;SDL_PushEvent(&e);e.type=SDL_MOUSEBUTTONDOWN;e.button.button=SDL_BUTTON_LEFT;e.button.state=SDL_PRESSED;e.button.x=x;e.button.y=y;SDL_PushEvent(&e);e.type=SDL_MOUSEBUTTONUP;e.button.state=SDL_RELEASED;SDL_PushEvent(&e);}
  fprintf(stderr,"TCX_CONTROL command=%s tick=%u\n",command,now);
 }}
 n=realPoll(event);if(n&&event&&event->type==SDL_KEYUP&&held&&event->key.keysym.sym==lastKey){SDL_SetModState(KMOD_NONE);held=KMOD_NONE;}return n;
}
