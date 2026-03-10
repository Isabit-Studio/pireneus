#ifndef _PLATFORMS_H_
#define _PLATFORMS_H_

int create_window(const char*, int, int); // title, width, height
int handle_events(); // returns 0 if the window should be closed, 1 otherwise
void destroy_window(); // destroys the window and cleans up resources

#endif // _PLATFORMS_H_