#include <stdio.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/audioio.h>
#include <sys/ioctl.h>
int main(int argc,char **argv) {
 audio_info_t info,change;int fd=open("/dev/audioctl",O_RDWR);
 if(fd<0||ioctl(fd,AUDIO_GETINFO,&info)<0){perror("audio");return 1;}
 printf("before port=%u available=%u gain=%u speaker=%u headphone=%u lineout=%u\n",info.play.port,info.play.avail_ports,info.play.gain,AUDIO_SPEAKER,AUDIO_HEADPHONE,AUDIO_LINE_OUT);
 if(argc>1&&!strcmp(argv[1],"rear")){
  unsigned port=info.play.avail_ports&(AUDIO_HEADPHONE|AUDIO_LINE_OUT);
  if(!(info.play.avail_ports&port)){fprintf(stderr,"Rear audio output unavailable\n");return 2;}
  AUDIO_INITINFO(&change);change.play.port=port;
  if(ioctl(fd,AUDIO_SETINFO,&change)<0){perror("set port");return 1;}
 }
 if(ioctl(fd,AUDIO_GETINFO,&info)<0)return 1;
 printf("after port=%u gain=%u muted=%u\n",info.play.port,info.play.gain,info.output_muted);close(fd);return 0;
}
