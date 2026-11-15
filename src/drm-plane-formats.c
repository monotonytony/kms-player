#include <stdio.h>
#include <fcntl.h>
#include <xf86drm.h>
#include <xf86drmMode.h>
static void fourcc(unsigned int f, char *o){o[0]=f&0xff;o[1]=(f>>8)&0xff;o[2]=(f>>16)&0xff;o[3]=(f>>24)&0xff;o[4]=0;}
int main(void){
  int fd=open("/dev/dri/card0",O_RDWR);
  if(fd<0){perror("open");return 1;}
  drmModePlaneRes *pr=drmModeGetPlaneResources(fd);
  if(!pr){perror("planes");return 1;}
  for(unsigned i=0;i<pr->count_planes;i++){
    drmModePlane *p=drmModeGetPlane(fd,pr->planes[i]);
    if(!p) continue;
    printf("plan %u: ",p->plane_id);
    for(unsigned j=0;j<p->count_formats;j++){char s[5];fourcc(p->formats[j],s);printf("%s ",s);}
    printf("\n");
    drmModeFreePlane(p);
  }
  return 0;
}
