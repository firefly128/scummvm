#define __EXTENSIONS__ 1
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/types.h>
#include <sys/time.h>
#include <sys/ioctl.h>
#include <sys/audioio.h>
int main(int argc,char **argv){int seconds=argc>1?atoi(argv[1]):20;int fd=open("/dev/audioctl",O_RDWR);audio_info_t i,u;hrtime_t start=gethrtime(),last=start;unsigned int prev=0;double maxgap=0;int initialized=0;if(fd<0){perror("open");return 1;}if(argc>2&&!strcmp(argv[2],"clear")){AUDIO_INITINFO(&u);u.play.error=0;int rc=ioctl(fd,AUDIO_SETINFO,&u);printf("CLEAR_ERROR rc=%d\n",rc);}while((gethrtime()-start)<(hrtime_t)seconds*1000000000LL){hrtime_t now=gethrtime();if(ioctl(fd,AUDIO_GETINFO,&i)<0){perror("getinfo");return 1;}if(!initialized){prev=i.play.samples;last=now;initialized=1;}if(i.play.samples!=prev){double gap=(now-last)/1000000.;if(gap>maxgap)maxgap=gap;last=now;prev=i.play.samples;}printf("AUDIO_WATCH ms=%.1f samples=%u error=%u active=%u buffer=%u max_counter_gap_ms=%.1f\n",(now-start)/1000000.,i.play.samples,i.play.error,i.play.active,i.play.buffer_size,maxgap);fflush(stdout);usleep(100000);}close(fd);return 0;}
