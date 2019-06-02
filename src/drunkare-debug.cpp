#include <string>
#include <iostream>
#include <fstream>
#include <queue>
#include <deque>
// #include <thread>
#include <pthread.h>
#include <memory>

// Tizen libraries
#include <locations.h>
#include <sensor.h>
#include <privacy_privilege_manager.h>
#include <efl_util.h>
#include <service_app.h>
#include <app_alarm.h>
#include <app_control.h>
#include <device/power.h>
#include <Ecore.h>
#include <curl/curl.h>

#include "drunkare-debug.h"
#include "queue.h"
#include "data.h"

#define NUM_SENSORS 2
#define NUM_CHANNELS 3
#define NUM_CONTEXTS 4
#define DURATION 12 // seconds
#define CONTEXT_DURATION 60 * 60 * 24 // seconds
#define MAX_MEASURE_ID (CONTEXT_DURATION / DURATION)
#define ACCELEROMETER 0
#define GYROSCOPE 1

static location_service_state_e service_state;

using TMeasure = Measure<NUM_CHANNELS, DURATION>;

static std::vector<std::string> btnLabels = {"start", "stop"};
static std::vector<std::pair<int, int>> btnOfs = {{50, 110}, {190, 110}};

struct appdata_s {
  Evas_Object *win;
  Evas_Object *conform;
  Evas_Object *label;
  std::vector<Evas_Object *> startBtn;
  std::vector<Evas_Object *> stopBtn;
  Evas_Object* lmCreateBtn;
  Evas_Object* lmDestroyBtn;
  std::string response; // TODO: delete this

  // Extra app data
  bool _isMeasuring;
  int _numWrite;
  int _context;
  sensor_h sensors[NUM_SENSORS];
  sensor_listener_h listners[NUM_SENSORS];
  int _deviceSamplingRate = 10;
  std::vector<int> _measureId;
  std::vector<int> _doneMeasureId;
  std::deque<std::unique_ptr<TMeasure>> tMeasures[NUM_SENSORS];
  pthread_t fsWorker; // format and write to file system
  Queue<TMeasure> queue;

  std::string filepath;
  std::string pathname;

  location_manager_h location;
  Ecore_Timer *timer;
  int alarm_id;

  bool location_available;
  bool location_service_is_running;
  double user_latitude;
  double user_longitude;

  appdata_s(): win(nullptr), location(nullptr), location_available(false),
               location_service_is_running(false) {}
};

static void win_delete_request_cb(void *data, Evas_Object *obj,
                                  void *event_info) {
  ui_app_exit();
}

static void
win_back_cb(void *data, Evas_Object *obj, void *event_info)
{
	appdata_s *ad = (appdata_s *)data;
        /* Let window go to hide state. */
	elm_win_lower(ad->win);
}

/*
 * Location manager functions ================================
 */

void __position_updated_cb(double latitude, double longitude, double altitude,
                           time_t timestamp, void *user_data)
{
  char message[128];
  int ret = 0;
  appdata_s *ad = (appdata_s *) user_data;

  ad->location_available = true;
  ad->user_latitude = latitude;
  ad->user_longitude = longitude;

  sprintf(message, "(%f,%f)\n", ad->user_latitude, ad->user_longitude);
  elm_object_text_set(ad->label, message);
  evas_object_show(ad->label);

  dlog_print(DLOG_DEBUG, LOG_TAG, "%d %d",
             ad->_numWrite, ad->_deviceSamplingRate);

  dlog_print(DLOG_DEBUG, LOG_TAG, "[%ld] lat[%f] lon[%f] alt[%f] (ret=%d)",
             timestamp, latitude, longitude, altitude, ret);


  /*
   * Let's do CURL! =========================================
   */
  CURL* curl;
  CURLcode res;
  unsigned long long curl_timestamp = (unsigned long long)time(nullptr); // Hack!

  std::ostringstream oss;
  oss << "{\"timestamp\":" << curl_timestamp << ","
      << "\"user_id\":0," /* dummy user_id */
      << "\"latitude\":" << ad->user_latitude << ","
      << "\"longitude\":" << ad->user_longitude << "}";
  std::string jsonObj = oss.str();

  // {url}:{port}/location
  std::string url = "localhost:8080/location/";

  curl_global_init(CURL_GLOBAL_ALL);
  curl = curl_easy_init();

  struct curl_slist *headers = NULL;
  headers = curl_slist_append(headers, "Content-Type: application/json");
  headers = curl_slist_append(headers, "charsets: utf-8");

  curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
  curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
  curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
  curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonObj.c_str());

  res = curl_easy_perform(curl);
  if (res != CURLE_OK) {
    pthread_exit(NULL);

    for (auto button : ad->startBtn) {
      elm_object_disabled_set(button, EINA_FALSE);
    }
  }
  /* end CURL... */

  curl_easy_cleanup(curl);
  curl_global_cleanup();
  usleep(3000);

  // sprintf(message, "<align=left>[%ld] lat[%f] lon[%f] alt[%f]
  // (ret=%d)\n</align>", 		timestamp, latitude, longitude, altitude, ret);
  // elm_entry_entry_set(ad->label, message);

  // stop_location_service(ad);
}

static void
__state_changed_cb(location_service_state_e state, void* user_data)
{
  double altitude;
  double latitude;
  double longitude;
  double climb;
  double direction;
  double speed;
  double horizontal;
  double vertical;
  location_accuracy_level_e level;
  time_t timestamp;
  int ret;

  appdata_s *ad = (appdata_s *)user_data;

  if (state == LOCATIONS_SERVICE_ENABLED) {
    dlog_print(DLOG_INFO, LOG_TAG, "[+] LOCATIONS_SERVICE_ENABLED");
    ret = location_manager_get_location(
        ad->location, &altitude, &latitude, &longitude, &climb, &direction, &speed,
        &level, &horizontal, &vertical, &timestamp);

    ad->location_available = true;
    ad->user_longitude = longitude;
    ad->user_latitude = latitude;

  } else {
    dlog_print(DLOG_INFO, LOG_TAG, "[+] LOCATIONS_SERVICE_DISABLED");
  }
}

static void
create_location_service(void *data)
{
  appdata_s *ad = (appdata_s *)data;
  location_manager_h manager;
  int ret;

  /* Create the location service to use all positioning source */
  ret = location_manager_create(LOCATIONS_METHOD_HYBRID, &manager);
  if (ret != LOCATIONS_ERROR_NONE) {
    dlog_print(DLOG_INFO, LOG_TAG,
               "[-] location_manager_create() failed. (%d)", ret);
  } else {
    ad->location = manager;
    ret = location_manager_set_position_updated_cb(manager,
                                                   __position_updated_cb,
                                                   12 /* Period */,
                                                   data /* Really? */);

    if (ret != LOCATIONS_ERROR_NONE) {
      dlog_print(DLOG_INFO, LOG_TAG,
                 "[-] location_manager_set_position_updated_cb() failed. (%d)", ret);
    }
    ret = LOCATIONS_ERROR_NONE;

    ret = location_manager_set_service_state_changed_cb(manager,
                                                        __state_changed_cb,
                                                        data /* Really? */);

    if (ret != LOCATIONS_ERROR_NONE) {
      dlog_print(DLOG_INFO, LOG_TAG,
                 "[-] location_manager_set_state_changed_cb() failed. (%d)", ret);
    }
  }
}

static void
destroy_location_service(void *data)
{
  appdata_s *ad = (appdata_s *)data;
  int ret;

  ret = location_manager_destroy(ad->location);
  if (ret != LOCATIONS_ERROR_NONE)
    dlog_print(DLOG_INFO, LOG_TAG,
               "[-] location_manager_destroy() failed. (%d)", ret);
  else
    ad->location = nullptr;
}

/* Create an app control for the alarm */
// static bool
// _initialize_alarm(void *data)
// {
//   appdata_s *ad = (appdata_s *) data;
// 
//   int ret;
//   int DELAY = 120;
//   int alarm_id;
// 
//   app_control_h app_control = nullptr;
//   ret = app_control_create(&app_control);
//   ret = app_control_set_operation(app_control, APP_CONTROL_OPERATION_DEFAULT);
// 
//   /* Set app_id as the name of the application */
//   ret = app_control_set_app_id(app_control, "com.drunkare.debug");
// 
//   /* Set the key ("location") and value ("stop") of a bundle */
//   ret = app_control_add_extra_data(app_control, "location", "stop");
// 
//   /* In order to be called after DELAY */
//   ret = alarm_schedule_once_after_delay(app_control, DELAY, &alarm_id);
//   if (ret != ALARM_ERROR_NONE) {
//     char *err_msg = get_error_message(ret);
//     dlog_print(DLOG_ERROR, LOG_TAG,
//                "alarm_schedule_once_after_delay() failed.(%d)", ret);
//     dlog_print(DLOG_INFO, LOG_TAG, "%s", err_msg);
// 
//     return false;
//   }
// 
//   ad->alarm_id = alarm_id;
// 
//   ret = app_control_destroy(app_control);
//   if (ret != APP_CONTROL_ERROR_NONE)
//     dlog_print(DLOG_ERROR, LOG_TAG, "app_control_destroy() failed.(%d)", ret);
//   else
//     dlog_print(DLOG_DEBUG, LOG_TAG, "Set the triggered time with alarm_id: %d",
//                ad->alarm_id);
// 
//   return true;
// }

static void
start_location_service(void *data)
{
  dlog_print(DLOG_INFO, LOG_TAG, "[+] start_location_service()");

  appdata_s *ad = (appdata_s *)data;
  int ret = 0;

  if (ad->location_service_is_running) {
    /* Location servuce is always running */
    dlog_print(DLOG_WARN, LOG_TAG,
               "[-] start_location_service() is invoked while service is already running");
    return;
  }

  ret = location_manager_start(ad->location);
  if (ad->location) {
    if (ret != LOCATIONS_ERROR_NONE) {
      dlog_print(DLOG_ERROR, LOG_TAG, "location_manager_start() failed: %d",
                 ret);
    } else {
      dlog_print(DLOG_DEBUG, LOG_TAG, "location service was started");
      ad->location_service_is_running = true;
    }


    /* Create a app control for the alarm */
    // ret = _initialize_alarm(ad);
  }
}

static void
stop_location_service(void *data)
{
  dlog_print(DLOG_INFO, LOG_TAG, "[+] stop_location_service()");

  appdata_s *ad = (appdata_s *)data;
  int ret = 0;

  if (!ad->location_service_is_running) {
    /* Location servuce is NOT running */
    dlog_print(DLOG_WARN, LOG_TAG,
               "[-] stop_location_service() is invoked while service is not running");
    return;
  }

  if (ad->location) {
    ret = location_manager_stop(ad->location);
    if (ret != LOCATIONS_ERROR_NONE) {
      dlog_print(DLOG_ERROR, LOG_TAG, "location_manager_stop() failed: %d",
                 ret);
    } else {
      dlog_print(DLOG_DEBUG, LOG_TAG, "location service was stopped.");
      ad->location_service_is_running = false;
    }

    ad->location_available = false;

    // if (ad->alarm_id) {
    //   alarm_cancel(ad->alarm_id);
    //   ad->alarm_id = 0;
    // }
  }
}


/*
 * Privacy-related Permissions ================================
 */

void
app_request_response_cb(ppm_call_cause_e cause, ppm_request_result_e result,
                        const char *privilege, void *user_data)
{
  if (cause == PRIVACY_PRIVILEGE_MANAGER_CALL_CAUSE_ERROR) {
    /* Log and handle errors */
    return;
  }

  appdata_s *ad = (appdata_s *)user_data;
  switch (result) {
  case PRIVACY_PRIVILEGE_MANAGER_REQUEST_RESULT_ALLOW_FOREVER:
    /* Update UI and start accessing protected functionality */

    dlog_print(DLOG_INFO, LOG_TAG,
               "[+] PRIVACY_PRIVILEGE_MANAGER_REQUEST_RESULT_ALLOW_FOREVER");

    if (!ad->location)
      /* Create location manager once */
      create_location_service((void *)ad);

    if (ad->location) {
      start_location_service((void *)ad);
    }
    break;
  case PRIVACY_PRIVILEGE_MANAGER_REQUEST_RESULT_DENY_FOREVER:
    /* Show a message and terminate the application */
    // exit(1);
    break;
  case PRIVACY_PRIVILEGE_MANAGER_REQUEST_RESULT_DENY_ONCE:
    /* Show a message with explanation */
    // exit(1);
    break;
  }
}

void
app_check_and_request_permission(void *user_data)
{
  ppm_check_result_e result;
  const char *privilege = "http://tizen.org/privilege/location";

  dlog_print(DLOG_INFO, LOG_TAG, "[+] app_check_and_request_permission()");

  int ret = ppm_check_permission(privilege, &result);

  if (ret == PRIVACY_PRIVILEGE_MANAGER_ERROR_NONE) {
    switch (result) {
    case PRIVACY_PRIVILEGE_MANAGER_CHECK_RESULT_ALLOW:
      /* Updata UI and start accessing protected functionality */
      break;
    case PRIVACY_PRIVILEGE_MANAGER_CHECK_RESULT_DENY:
      /* Show a message and terminate the application */
      break;
    case PRIVACY_PRIVILEGE_MANAGER_CHECK_RESULT_ASK:
      ret = ppm_request_permission(privilege, app_request_response_cb, user_data);
      break;
    }
  } else {
    /* ret != PRIVACY_PRIVILEGE_MANAGER_ERROR_NONE */
    /* Handle errors! */
  }
}


//
// Main function for `netWorker`.
//   1. Dequeue a `Measure` from `ad->queue`
//   2. Format POST fields
//   3. Send POST request to <hostname:port>
//   4. Repeat
//
static void* netWorkerJob(void* data) {
  appdata_s *ad = (appdata_s *)data;
  CURL *curl;
  CURLcode res;

  while (true) {
    auto tMeasure = ad->queue.dequeue();

    if (!tMeasure)
      break;

    std::string jsonObj = tMeasure->formatJson();

    // {url}:{port}/data
    std::string url = "localhost:8080/data/";

    /* Curl POST */
    curl_global_init(CURL_GLOBAL_ALL);
    curl = curl_easy_init();

    struct curl_slist *headers = NULL;
    headers = curl_slist_append(headers, "Content-Type: application/json");
    headers = curl_slist_append(headers, "charsets: utf-8");

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "POST");
    curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
    curl_easy_setopt(curl, CURLOPT_POSTFIELDS, jsonObj.c_str());

    res = curl_easy_perform(curl);
    if (res != CURLE_OK) {
    	pthread_exit(NULL);

        for (auto button : ad->startBtn) {
          elm_object_disabled_set(button, EINA_FALSE);
        }
    }

    curl_easy_cleanup(curl);
    curl_global_cleanup();
    usleep(3000);
  }

  return nullptr;
}

static void startMeasurement(appdata_s *ad);
static void stopMeasurement(appdata_s *ad);

void sensorCb(sensor_h sensor, sensor_event_s *event, void *user_data)
{
  appdata_s *ad = (appdata_s *)user_data;
  int sensor_type;
  sensor_type_e type;
  sensor_get_type(sensor, &type);
  std::vector<float> values;

  switch (type) {
  case SENSOR_ACCELEROMETER:
    sensor_type = ACCELEROMETER;
    break;
  case SENSOR_GYROSCOPE:
    sensor_type = GYROSCOPE;
    break;
  default:
    return;
  }

  // Save temporary value array
  for (int i = 0; i < NUM_CHANNELS; i++) {
    values.push_back(event->values[i]);
  }

  // Check tMeasures deque
  if (ad->tMeasures[sensor_type].empty()) {
    unsigned long long timestamp = (unsigned long long)time(nullptr);
    ad->tMeasures[sensor_type].push_back(
        std::make_unique<TMeasure>(ad->_measureId[sensor_type]++,
                                   sensor_type, ad->_context, timestamp));
  }

  // Tick (store values in Measure.data every periods)
  ad->tMeasures[sensor_type].front()->tick(values);

  // Check Measure->_done and enqueue
  if (ad->tMeasures[sensor_type].front()->_done) {
    ad->_doneMeasureId[sensor_type] = ad->tMeasures[sensor_type].front()->_id;

    ad->queue.enqueue(std::move(ad->tMeasures[sensor_type].front()));
    ad->tMeasures[sensor_type].pop_front();

    // Check termination condition
    if (ad->_doneMeasureId[sensor_type] >= MAX_MEASURE_ID) {
      bool allFinished = true;

      for (auto doneId : ad->_doneMeasureId) {
        allFinished &= (doneId >= MAX_MEASURE_ID);
      }

      if (allFinished) {
        stopMeasurement(ad);
        return;
      }
    }
  }
}

static void startMeasurement(appdata_s *ad)
{
  /* NOTE that is is very less likely to be here, since we disable
     button while measuring */
  if (ad->_isMeasuring) {
    /* sensor servuce is already running */
    dlog_print(DLOG_WARN, LOG_TAG,
               "[-] startMeasurement() is invoked while sensor service is already running");
    return;
  }


  ad->queue.clear();

  // See https://stackoverflow.com/questions/49752776
  device_power_request_lock(POWER_LOCK_CPU, 0);

  // Create thread here
  if (pthread_create(&ad->fsWorker, nullptr, netWorkerJob, (void *)ad) < 0) {
    // dlog_print(DLOG_ERROR, "btnClickedCb", "[-] pthread_create()");
    return;
  };

  // See https://stackoverflow.com/questions/49752776
  for (int i = 0; i < NUM_SENSORS; i++) {
    sensor_listener_set_option(ad->listners[i], SENSOR_OPTION_ALWAYS_ON);
    sensor_listener_set_attribute_int(ad->listners[i], SENSOR_ATTRIBUTE_PAUSE_POLICY, SENSOR_PAUSE_NONE);
    sensor_listener_set_event_cb(ad->listners[i], ad->_deviceSamplingRate,
                                 sensorCb, ad);
    sensor_listener_start(ad->listners[i]);
  }
  ad->_isMeasuring = true;
}

static void stopMeasurement(appdata_s *ad)
{
  if (!ad->_isMeasuring) {
    /* sensor servuce is NOT running */
    dlog_print(DLOG_WARN, LOG_TAG,
               "[-] stopMeasurement() is invoked while sensor service is not running");
    return;
  }

  ad->_isMeasuring = false;

  device_power_release_lock(POWER_LOCK_CPU);

  for (int i = 0; i < NUM_SENSORS; i++) {
    sensor_listener_stop(ad->listners[i]);
  }

  ad->queue.forceDone();
  pthread_join(ad->fsWorker, nullptr);

  for (int i = 0; i < ad->_measureId.size(); i++) {
    ad->_measureId[i] = 0;
    ad->_doneMeasureId[i] = -1;
    // dlog_print(DLOG_DEBUG, "stopMeasurement", "[+] ad->_measureId[%d] = %d", i,
    //            ad->_measureId[i]);
  }

  for (auto button : ad->startBtn) {
    elm_object_disabled_set(button, EINA_FALSE);
  }

  // Set screen to default mode
  // efl_util_set_window_screen_mode(ad->win, EFL_UTIL_SCREEN_MODE_DEFAULT);
}

static void startBtnClickedCb(void *data, Evas_Object *obj, void *event_info)
{
  appdata_s* ad = (appdata_s *)data;

  // 1. Set screen always on (This is due to hardware limitation)
  // efl_util_set_window_screen_mode(ad->win, EFL_UTIL_SCREEN_MODE_ALWAYS_ON);

  ad->_context = 0;

  if (!ad->_isMeasuring) {
    for (auto button : ad->startBtn) {
      elm_object_disabled_set(button, EINA_TRUE);
    }

    startMeasurement(ad);
  }

  /* Used to restart location service stopped by user */
  start_location_service(data);
}

static void stopBtnClickedCb(void *data, Evas_Object *obj, void *event_info)
{
  stopMeasurement((appdata_s*)data);

  stop_location_service(data);
}

static void
init_buttons(appdata_s *ad,
             void (*start_cb)(void *data, Evas_Object *obj, void *event_info),
             void (*stop_cb)(void *data, Evas_Object *obj, void *event_info)) {
  auto startBtn =  elm_button_add(ad->win);
  evas_object_smart_callback_add(startBtn, "clicked", start_cb, ad);
  evas_object_move(startBtn, btnOfs[0].first, btnOfs[0].second);
  evas_object_resize(startBtn, 120, 120);
  elm_object_text_set(startBtn, btnLabels[0].c_str());
  evas_object_show(startBtn);
  ad->startBtn.push_back(startBtn);

  auto stopBtn =  elm_button_add(ad->win);
  evas_object_smart_callback_add(stopBtn, "clicked", stop_cb, ad);
  evas_object_move(stopBtn, btnOfs[1].first, btnOfs[1].second);
  evas_object_resize(stopBtn, 120, 120);
  elm_object_text_set(stopBtn, btnLabels[1].c_str());
  evas_object_show(stopBtn);
  ad->stopBtn.push_back(stopBtn);
}

static void
create_base_gui(appdata_s *ad)
{
	/* Window */
	/* Create and initialize elm_win.
	   elm_win is mandatory to manipulate window. */
	ad->win = elm_win_util_standard_add(PACKAGE, PACKAGE);
	elm_win_autodel_set(ad->win, EINA_TRUE);

	if (elm_win_wm_rotation_supported_get(ad->win)) {
		int rots[4] = { 0, 90, 180, 270 };
		elm_win_wm_rotation_available_rotations_set(ad->win, (const int *)(&rots), 4);
	}

	evas_object_smart_callback_add(ad->win, "delete,request", win_delete_request_cb, NULL);
	eext_object_event_callback_add(ad->win, EEXT_CALLBACK_BACK, win_back_cb, ad);

	/* Conformant */
	/* Create and initialize elm_conformant.
	   elm_conformant is mandatory for base gui to have proper size
	   when indicator or virtual keypad is visible. */
	ad->conform = elm_conformant_add(ad->win);
	elm_win_indicator_mode_set(ad->win, ELM_WIN_INDICATOR_SHOW);
	elm_win_indicator_opacity_set(ad->win, ELM_WIN_INDICATOR_OPAQUE);
	evas_object_size_hint_weight_set(ad->conform, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
	elm_win_resize_object_add(ad->win, ad->conform);
	evas_object_show(ad->conform);

	/* Label */
	/* Create an actual view of the base gui.
	   Modify this part to change the view. */
	ad->label = elm_label_add(ad->conform);
	elm_object_text_set(ad->label, "<align=center>Hello Tizen</align>");
	evas_object_size_hint_weight_set(ad->label, EVAS_HINT_EXPAND, EVAS_HINT_EXPAND);
	elm_object_content_set(ad->conform, ad->label);

        /* Custom initializations are here! */
        ad->response = ""; // TODO: delete this
        ad->_isMeasuring = false;
        init_buttons(ad, startBtnClickedCb, stopBtnClickedCb);
        sensor_get_default_sensor(SENSOR_ACCELEROMETER, &ad->sensors[ACCELEROMETER]);
        sensor_get_default_sensor(SENSOR_GYROSCOPE, &ad->sensors[GYROSCOPE]);
        for (int i = 0; i < NUM_SENSORS; i++) {
          sensor_create_listener(ad->sensors[i], &ad->listners[i]);

          // Initialize per-sensor measure IDs
          ad->_measureId.push_back(0);
          ad->_doneMeasureId.push_back(-1);
        }

        ad->filepath = std::string(app_get_data_path());
        ad->pathname = ad->filepath + std::string("data.csv");

        /* Show window after base gui is set up */
	evas_object_show(ad->win);
}

static bool
app_create(void *data)
{
  /* Hook to take necessary actions before main event loop starts
     Initialize UI resources and application's data
     If this function returns true, the main loop of application starts
     If this function returns false, the application is terminated */
  appdata_s *ad = (appdata_s *)data;

  create_base_gui(ad);

  /* Ask for users to agree on location access */
  app_check_and_request_permission(data);

  // create_location_service(ad);

  // if (ad->location)
  //   start_location_service(ad);

  return true;
}

static void
app_control(app_control_h app_control, void *data)
{
  // /* Handle the launch request. */
  // appdata_s *ad = (appdata_s *)data;
  // char *value = NULL;

  // dlog_print(DLOG_DEBUG, LOG_TAG, "app_control was called.");
  // /* Check whether the key and value of the bundle are as expected */
  // if (app_control_get_extra_data(app_control, "location", &value) ==
  //     APP_CONTROL_ERROR_NONE) {
  //   if (!strcmp(value, "stop")) {
  //     if (ad->location)
  //       stop_location_service(ad);
  //   }
  //   free(value);
  // }
}

static void
app_pause(void *data)
{
  /* Take necessary actions when application becomes invisible. */
  appdata_s *ad = (appdata_s *)data;

  // stop_location_service(ad);
}

static void
app_resume(void *data)
{
  /* Take necessary actions when application becomes visible. */
  appdata_s *ad = (appdata_s *)data;

  app_check_and_request_permission(data);

  start_location_service(ad);
}

static void
app_terminate(void *data)
{
  /* Release all resources. */
  appdata_s *ad = (appdata_s *)data;

  stop_location_service(data);

  if (ad->location)
    destroy_location_service(ad);
}

static void
ui_app_lang_changed(app_event_info_h event_info, void *user_data)
{
	/*APP_EVENT_LANGUAGE_CHANGED*/
	char *locale = NULL;
	system_settings_get_value_string(SYSTEM_SETTINGS_KEY_LOCALE_LANGUAGE, &locale);
	elm_language_set(locale);
	free(locale);
	return;
}

static void
ui_app_orient_changed(app_event_info_h event_info, void *user_data)
{
	/*APP_EVENT_DEVICE_ORIENTATION_CHANGED*/
	return;
}

static void
ui_app_region_changed(app_event_info_h event_info, void *user_data)
{
	/*APP_EVENT_REGION_FORMAT_CHANGED*/
}

static void
ui_app_low_battery(app_event_info_h event_info, void *user_data)
{
	/*APP_EVENT_LOW_BATTERY*/
}

static void
ui_app_low_memory(app_event_info_h event_info, void *user_data)
{
	/*APP_EVENT_LOW_MEMORY*/
}

int
main(int argc, char *argv[])
{
  appdata_s ad;
	int ret = 0;

	ui_app_lifecycle_callback_s event_callback = {0,};
	app_event_handler_h handlers[5] = {NULL, };

        bool supported[NUM_SENSORS];
	sensor_is_supported(SENSOR_ACCELEROMETER, &supported[ACCELEROMETER]);
	sensor_is_supported(SENSOR_GYROSCOPE, &supported[GYROSCOPE]);
	if (!supported[ACCELEROMETER] || !supported[GYROSCOPE]) {
		/* Accelerometer is not supported on the current device */
		return 1;
	}

        // Initialize sensor handles
        // 	sensor_get_default_sensor(SENSOR_ACCELEROMETER, &sensors[ACCELEROMETER]);
        // 	sensor_get_default_sensor(SENSOR_GYROSCOPE, &sensors[GYROSCOPE]);
        // 
        //         // Initialize sensor listeners
        //         for (int i = 0; i < NUM_SENSORS; i++) {
        //           sensor_create_listener(sensors[sensor_index],
        //                                  &listeners[sensor_index]);
        //         }

	event_callback.create = app_create;
	event_callback.terminate = app_terminate;
	event_callback.pause = app_pause;
	event_callback.resume = app_resume;
	event_callback.app_control = app_control;

	ui_app_add_event_handler(&handlers[APP_EVENT_LOW_BATTERY], APP_EVENT_LOW_BATTERY, ui_app_low_battery, &ad);
	ui_app_add_event_handler(&handlers[APP_EVENT_LOW_MEMORY], APP_EVENT_LOW_MEMORY, ui_app_low_memory, &ad);
	ui_app_add_event_handler(&handlers[APP_EVENT_DEVICE_ORIENTATION_CHANGED], APP_EVENT_DEVICE_ORIENTATION_CHANGED, ui_app_orient_changed, &ad);
	ui_app_add_event_handler(&handlers[APP_EVENT_LANGUAGE_CHANGED], APP_EVENT_LANGUAGE_CHANGED, ui_app_lang_changed, &ad);
	ui_app_add_event_handler(&handlers[APP_EVENT_REGION_FORMAT_CHANGED], APP_EVENT_REGION_FORMAT_CHANGED, ui_app_region_changed, &ad);

	ret = ui_app_main(argc, argv, &event_callback, &ad);
	if (ret != APP_ERROR_NONE) {
		dlog_print(DLOG_ERROR, LOG_TAG, "app_main() is failed. err = %d", ret);
	}

	return ret;
}
