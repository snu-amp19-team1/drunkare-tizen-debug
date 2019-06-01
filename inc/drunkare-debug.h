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

void _position_updated_cb(double latitude, double longtitude, double altitude,
		time_t timestamp, void *data);
static void _clicked_start_cb(void *data, Evas_Object *obj EINA_UNUSED,
		void *event_info EINA_UNUSED);
static void _clicked_stop_cb(void *data, Evas_Object *obj EINA_UNUSED,
		void *event_info EINA_UNUSED);

#endif /* __drunkare-debug_H__ */
