#ifndef NETPLAY_H
#define NETPLAY_H

#include <stdbool.h>

void Netplay_SetPlayer(int player);
int Netplay_GetPlayer();
void Netplay_Begin();
void Netplay_Run();

#endif
