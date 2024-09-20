#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>
#include <gui/gui.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <gui/elements.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/dialog_ex.h>
#include <locale/locale.h>

#define TAG "WIFI_MAP"
#define FILE_NAME "wifi_map_data.csv"
#define MAX_AP_LIST 20
// #define WORKER_EVENTS_MASK (WorkerEventStop | WorkerEventRx)
#define BAUD_RATE 115200 
#define RX_BUFFER_SIZE 1024

// Screen is 128x64 px
#define SCREEN_WIDTH 128
#define SCREEN_HEIGHT 64

#define WORKER_EVENTS_MASK                                                                 \
    (WorkerEventStop | WorkerEventRxData | WorkerEventRxIdle | WorkerEventRxOverrunError | \
     WorkerEventRxFramingError | WorkerEventRxNoiseError)


typedef struct {
    Gui* gui;
    NotificationApp* notification;
    ViewDispatcher* view_dispatcher;
    View* view;
    FuriThread* worker_thread;
    FuriStreamBuffer* rx_stream;
    File* file;
    FuriHalSerialHandle* serial_handle;
} WiFiMapApp;

typedef struct WifiMapModel WifiMapModel;

typedef struct {
        FuriString* line;
} StringList;

struct WifiMapModel {
        StringList* lines[MAX_AP_LIST];
        int lnrdy;
        int rdy;
        int cntr;
};

// typedef enum {
//     WorkerEventReserved = (1 << 0), 
//     WorkerEventStop = (1 << 1),
//     WorkerEventRx = (1 << 2),
// } WorkerEventFlags;

typedef enum {
    WorkerEventReserved = (1 << 0), // Reserved for StreamBuffer internal event
    WorkerEventStop = (1 << 1),
    WorkerEventRxData = (1 << 2),
    WorkerEventRxIdle = (1 << 3),
    WorkerEventRxOverrunError = (1 << 4),
    WorkerEventRxFramingError = (1 << 5),
    WorkerEventRxNoiseError = (1 << 6),
} WorkerEventFlags;

const NotificationSequence sequence_notification = {
    &message_display_backlight_on,
    &message_green_255,
    &message_delay_10,
    NULL,
};

static File* open_file()
{
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    if (!storage_file_open(file, APP_DATA_PATH(FILE_NAME), FSAM_WRITE, FSOM_OPEN_APPEND)) {
        FURI_LOG_E(TAG, "Failed to open file");
    }
    return file;
}

static int32_t write_to_file(char data_line, File *file)
{
    char *data = (char *)malloc(sizeof(char) + 1);
    data[0] = data_line;
    if (!storage_file_write(file, data, (uint16_t)strlen(data))) {
        FURI_LOG_E(TAG, "Failed to write to file");
    }
    free(data);
    return 0;
}

static int32_t close_file(File *file)
{
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return 0;
}

static void retrieve_ap_ssid_distance(const char *data, char *apssid, char *dst)
{
        for (size_t i = 0; i < 2; i++) {
                apssid[i] = data[i];
        }

        int cntr = 0;
        for (size_t i = 9; i < strlen(data); i++) {
                if (data[i] == '.')
                        break;
                else
                        dst[cntr] = data[i];
                cntr++;
        }
}

static void uart_echo_view_draw_callback(Canvas* canvas, void* _model) 
{
    WifiMapModel* model = _model;
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontSecondary);
    int cntr = 0;
    for (size_t i = 0; i < MAX_AP_LIST; i++) {
        const char *line = furi_string_get_cstr(model->lines[i]->line);
        char apssid[2], dst[6];
        retrieve_ap_ssid_distance(line, apssid, dst);
        if (strlen(line) > 0) {
                int d = atoi(dst);
                canvas_draw_circle(canvas, 0, SCREEN_HEIGHT/2, d); 
                if (i%2) {
                        canvas_draw_str(canvas, d, (SCREEN_HEIGHT/2) + cntr, apssid);
                } else {
                        canvas_draw_str(canvas, d, (SCREEN_HEIGHT/2) - cntr, apssid);
                        cntr = cntr + 8;
                }
                if (i > 8)
                        cntr = 8;
        }
    }
    
    if (model->rdy) {
            canvas_clear(canvas);
            for (size_t i = 0; i < MAX_AP_LIST; i++) {
                    furi_string_reset(model->lines[i]->line);
            }
            model->rdy = 0;
            model->cntr = 0;
    }
}

static bool uart_echo_view_input_callback(InputEvent* event, void* context)
{
        UNUSED(event);
        UNUSED(context);
        return false;
}

static uint32_t uart_echo_exit(void* context)
{
        UNUSED(context);
        return VIEW_NONE;
}

static void uart_echo_on_irq_cb(FuriHalSerialHandle* handle, FuriHalSerialRxEvent data, void* context)
{
    furi_assert(context);
    UNUSED(handle);
    WiFiMapApp* app = context;
    volatile FuriHalSerialRxEvent event_copy = data;
    UNUSED(event_copy);

    WorkerEventFlags flag = 0;

    if(data & FuriHalSerialRxEventData) {
        uint8_t data = furi_hal_serial_async_rx(handle);
        furi_stream_buffer_send(app->rx_stream, &data, 1, 0);
    }

    if(data& FuriHalSerialRxEventIdle) {
        //idle line detected, packet transmission may have ended
        flag |= WorkerEventRxIdle;
    }

    //error detected
    if(data& FuriHalSerialRxEventFrameError) {
        flag |= WorkerEventRxFramingError;
    }
    if(data& FuriHalSerialRxEventNoiseError) {
        flag |= WorkerEventRxNoiseError;
    }
    if(data& FuriHalSerialRxEventOverrunError) {
        flag |= WorkerEventRxOverrunError;
    }

    furi_thread_flags_set(furi_thread_get_id(app->worker_thread), flag);
}

static void uart_push_to_list(WifiMapModel* model, const char data , WiFiMapApp* app)
{
        write_to_file((char) data,  app->file);
        if (!model->rdy) {
                if (data != '\n') 
                    furi_string_push_back(model->lines[model->cntr]->line, data);
                else 
                    model->cntr++;

                if (model->cntr == MAX_AP_LIST) 
                    model->rdy = 1;
        } 
}

static int32_t wifi_map_worker(void* context)
{
        FURI_LOG_I(TAG, "LoL0");
        furi_assert(context);
        WiFiMapApp* app = context;

        FURI_LOG_I(TAG, "LoL");
        while (1) {
                FURI_LOG_I(TAG, "LoL1");
            uint32_t events =
                furi_thread_flags_wait(WORKER_EVENTS_MASK, FuriFlagWaitAny, FuriWaitForever);
            furi_check((events & FuriFlagError) == 0);
                FURI_LOG_I(TAG, "LoL2");

            if (events & WorkerEventStop) { 
                    break;
            }
            if (events & WorkerEventRxData) {
                size_t length = 0;
                do {
                    uint8_t data[64];
                    length = furi_stream_buffer_receive(app->rx_stream, data, 64, 0);
                    if (length > 0) {
                        furi_hal_serial_tx(app->serial_handle, data, length);
                        with_view_model(
                            app->view,
                            WifiMapModel* model,
                            {
                                for (size_t i = 0; i < length; i++) {
                                    uart_push_to_list(model, data[i], app);
                                }
                            },
                            false);
                    }
                } while(length > 0);
                FURI_LOG_I(TAG, "LoL3");

                notification_message(app->notification, &sequence_notification);
                with_view_model(
                    app->view, WifiMapModel* model, { UNUSED(model); }, true);
            }
        if(events & WorkerEventRxIdle) {
            furi_hal_serial_tx(app->serial_handle, (uint8_t*)"\r\nDetect IDLE\r\n", 15);
        }

        if(events &
           (WorkerEventRxOverrunError | WorkerEventRxFramingError | WorkerEventRxNoiseError)) {
            if(events & WorkerEventRxOverrunError) {
                furi_hal_serial_tx(app->serial_handle, (uint8_t*)"\r\nDetect ORE\r\n", 14);
            }
            if(events & WorkerEventRxFramingError) {
                furi_hal_serial_tx(app->serial_handle, (uint8_t*)"\r\nDetect FE\r\n", 13);
            }
            if(events & WorkerEventRxNoiseError) {
                furi_hal_serial_tx(app->serial_handle, (uint8_t*)"\r\nDetect NE\r\n", 13);
            }
        }
        }

        return 0;
}

static WiFiMapApp* wifi_map_app_alloc() {
	FURI_LOG_D(TAG, "0_app_alloc");
    WiFiMapApp* app = malloc(sizeof(WiFiMapApp));
	FURI_LOG_D(TAG, "1_app_alloc");
    app->file = open_file();
	FURI_LOG_D(TAG, "2_app_alloc");
    app->rx_stream = furi_stream_buffer_alloc(2048, 1);
	FURI_LOG_D(TAG, "3_app_alloc");

    // Gui
    app->gui = furi_record_open(RECORD_GUI);
	FURI_LOG_D(TAG, "4_app_alloc");
    app->notification = furi_record_open(RECORD_NOTIFICATION);
	FURI_LOG_D(TAG, "5_app_alloc");

    // View dispatcher
    app->view_dispatcher = view_dispatcher_alloc();
	FURI_LOG_D(TAG, "6_app_alloc");
    // view_dispatcher_enable_queue(app->view_dispatcher);
	FURI_LOG_D(TAG, "7_app_alloc");
    view_dispatcher_attach_to_gui(app->view_dispatcher, app->gui, ViewDispatcherTypeFullscreen);
	FURI_LOG_D(TAG, "8_app_alloc");

    // Views
    app->view = view_alloc();
	FURI_LOG_D(TAG, "9_app_alloc");
    view_set_draw_callback(app->view, uart_echo_view_draw_callback);
	FURI_LOG_D(TAG, "10_app_alloc");
    view_set_input_callback(app->view, uart_echo_view_input_callback);
	FURI_LOG_D(TAG, "11_app_alloc");
    view_allocate_model(app->view, ViewModelTypeLocking, sizeof(WifiMapModel));
	FURI_LOG_D(TAG, "12_app_alloc");

    with_view_model(
        app->view,
        WifiMapModel* model,
        {
                for (size_t i = 0; i < MAX_AP_LIST; i++) {
                    model->lines[i] = malloc(sizeof(StringList));
                    model->lines[i]->line = furi_string_alloc();
                }
                model->lnrdy = 0;
                model->rdy = 0;
                model->cntr = 0;
        },
        true);
	FURI_LOG_D(TAG, "13_app_alloc");
    
    view_set_previous_callback(app->view, uart_echo_exit);
	FURI_LOG_D(TAG, "14_app_alloc");
    view_dispatcher_add_view(app->view_dispatcher, 0, app->view);
	FURI_LOG_D(TAG, "15_app_alloc");
    view_dispatcher_switch_to_view(app->view_dispatcher, 0);
	FURI_LOG_D(TAG, "16_app_alloc");

    // init rx stream
    // app->worker_thread = furi_thread_alloc_ex("UsbUartWorker", 1024, wifi_map_worker, app);
    // furi_assert(app->worker_thread);
	// FURI_LOG_D(TAG, "17_app_alloc");
	// FURI_LOG_D(TAG, "17_app_alloc_lol");
    // furi_thread_start(app->worker_thread);
	// FURI_LOG_D(TAG, "18_app_alloc");
	// FURI_LOG_D(TAG, "18_1_app_alloc");
    app->rx_stream = furi_stream_buffer_alloc(RX_BUFFER_SIZE, 1);
    app->worker_thread = furi_thread_alloc();
    furi_thread_set_name(app->worker_thread, "UsbUartWorker");
    furi_thread_set_stack_size(app->worker_thread, 1024);
    furi_thread_set_context(app->worker_thread, app);
    furi_thread_set_callback(app->worker_thread, wifi_map_worker);

    furi_check(app->worker_thread);

    furi_thread_start(app->worker_thread);

        FURI_LOG_D(TAG, "17_app_alloc");
    // Enable uart listener
    // TODO: This is failing :(
    app->serial_handle = furi_hal_serial_control_acquire(FuriHalSerialIdUsart);
    // app->serial_handle = furi_hal_serial_control_acquire(10);
	FURI_LOG_D(TAG, "19_app_alloc");
    furi_check(app->serial_handle);
	FURI_LOG_D(TAG, "20_app_alloc");
    furi_hal_serial_init(app->serial_handle, BAUD_RATE);
	FURI_LOG_D(TAG, "21_app_alloc");
    furi_hal_serial_async_rx_start(app->serial_handle, uart_echo_on_irq_cb, app, true);
	FURI_LOG_D(TAG, "22_app_alloc");

    return app;
}

static void wifi_map_app_free(WiFiMapApp* app) {
    furi_assert(app);

    // Serial free
    furi_hal_serial_deinit(app->serial_handle);
    furi_hal_serial_control_release(app->serial_handle);

    furi_thread_flags_set(furi_thread_get_id(app->worker_thread), WorkerEventStop);
    furi_thread_join(app->worker_thread);
    furi_thread_free(app->worker_thread);

    // Free views
    view_dispatcher_remove_view(app->view_dispatcher, 0);

    with_view_model(
        app->view,
        WifiMapModel * model,
        {
            for (size_t i = 0; i < MAX_AP_LIST; i++) {
                free(model->lines[i]);
            }
            free(model);
        },
        true);
    view_free(app->view);
    view_dispatcher_free(app->view_dispatcher);

    // Close gui record
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    app->gui = NULL;

    furi_stream_buffer_free(app->rx_stream);

    close_file(app->file);

    // Free rest
    free(app);
}

int32_t wifi_map_app(void *p){
	UNUSED(p);
	FURI_LOG_I(TAG, "wifi_map_app starting...");
	WiFiMapApp* app = wifi_map_app_alloc();
        DateTime* rtc = {0};
        furi_hal_rtc_get_datetime(rtc);
	FURI_LOG_I(TAG, "0");
        FuriString *datetime = furi_string_alloc();
	FURI_LOG_I(TAG, "1");
        furi_string_printf(datetime, "##### %d-%d-%d_%d:%d:%d #####\n", rtc->day, rtc->month, rtc->year, rtc->hour, rtc->minute, rtc->second);
	FURI_LOG_I(TAG, "2");
        if (!storage_file_write(app->file, furi_string_get_cstr(datetime), furi_string_size(datetime))) {
            FURI_LOG_E(TAG, "Failed to write to file");
        }
	FURI_LOG_I(TAG, "3");
        view_dispatcher_run(app->view_dispatcher);
	FURI_LOG_I(TAG, "4");
        furi_string_free(datetime);
	FURI_LOG_I(TAG, "5");
        free(rtc);
        wifi_map_app_free(app);
	return 0;
}

// Screen is 128x64 px
// x -->
// y
// |
// |
//
