#include <stdio.h>
#include <dlfcn.h>
int main(void){void*h=dlopen("/vendor/libglib.so",RTLD_NOW|RTLD_GLOBAL);if(!h){puts(dlerror());return 1;}void*(*get)(void)=dlsym(h,"g_tls_backend_get_default");int(*supports)(void*)=dlsym(h,"g_tls_backend_supports_tls");const char*(*name)(void*)=dlsym(h,"g_type_name_from_instance");if(!get||!supports||!name)return 2;void*b=get();printf("GIO_TLS_BACKEND=%s\nGIO_TLS_SUPPORTED=%d\n",name(b),supports(b));return 0;}
