#pragma once

#include "channel.h"

extern struct Channel* to_screen;
extern struct Channel* from_kbd;

void* run_window(void* args);