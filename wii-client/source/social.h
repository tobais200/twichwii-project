#ifndef WII_SOCIAL_H
#define WII_SOCIAL_H
#include <stddef.h>
int social_url(const char*slug,int sub,char*out,size_t cap);
const char*social_label(const char*slug);
int social_refresh(const char*slug);
int social_favorite(const char*slug);
int social_toggle_favorite(const char*slug);
void social_screen(const char*slug,int sub);
int social_home(char*out,size_t cap);
#ifdef SOCIAL_MOCK
extern int social_test_stage;
#endif
#endif
