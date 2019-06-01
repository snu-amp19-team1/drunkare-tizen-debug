#ifndef __drunkare-debug_H__
#define __drunkare-debug_H__

#include <app.h>
#include <Elementary.h>
#include <system_settings.h>
#include <efl_extension.h>
#include <dlog.h>

#ifdef  LOG_TAG
#undef  LOG_TAG
#endif
#define LOG_TAG "drunkare-debug"

#if !defined(PACKAGE)
#define PACKAGE "com.drunkare.debug"
#endif

#endif /* __drunkare-debug_H__ */
