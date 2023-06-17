#include <furi.h>
#include <furi_hal.h>
#include <storage/storage.h>

#define TAG "WIFI_MAP"
#define FILE_NAME "wifi_map_data.csv"

#include <gui/gui.h>
#include <notification/notification.h>
#include <notification/notification_messages.h>
#include <gui/elements.h>
#include <furi_hal_uart.h>
#include <furi_hal_console.h>
#include <gui/view_dispatcher.h>
#include <gui/modules/dialog_ex.h>

#define LINES_ON_SCREEN 6
#define COLUMNS_ON_SCREEN 21
#define WORKER_EVENTS_MASK (WorkerEventStop | WorkerEventRx)


typedef struct {
    Gui* gui;
    ViewPort* view_port;
    FuriThread* worker_thread;
    FuriStreamBuffer* rx_stream;
    File* file;
    FuriMessageQueue* event_queue;
    char c;
} WiFiMapApp;

typedef enum {
    WorkerEventReserved = (1 << 0), // Reserved for StreamBuffer internal event
    WorkerEventStop = (1 << 1),
    WorkerEventRx = (1 << 2),
} WorkerEventFlags;

File* open_file(){
    Storage* storage = furi_record_open(RECORD_STORAGE);
    File* file = storage_file_alloc(storage);

    if(!storage_file_open(file, APP_DATA_PATH(FILE_NAME), FSAM_WRITE, FSOM_OPEN_APPEND)) {
        FURI_LOG_E(TAG, "Failed to open file");
    }
    return file;
}

int32_t write_to_file(char data_line, File *file) {
    char *data = (char *)malloc(sizeof(char) + 1);
    data[0] = data_line;
    if (!storage_file_write(file, data, (uint16_t)strlen(data))) {
        FURI_LOG_E(TAG, "Failed to write to file");
    }
    free(data);
    return 0;
}

int32_t close_file(File *file) {
    storage_file_close(file);
    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);
    return 0;
}

static void wifi_map_view_draw_callback(Canvas* canvas, void* context) {
    WiFiMapApp *app = context;
    FURI_LOG_D(TAG, "%p", app->view_port);
    // Prepare canvas
    char *lol = "this";
    canvas_clear(canvas);
    canvas_set_color(canvas, ColorBlack);
    canvas_set_font(canvas, FontKeyboard);
    canvas_draw_str(canvas, 10, 10, lol);
}

static void wifi_map_view_input_callback(InputEvent* event, void* context) {
    WiFiMapApp* app = context;
    furi_message_queue_put(app->event_queue, event, FuriWaitForever);
}

static void uart_echo_on_irq_cb(UartIrqEvent ev, uint8_t data, void* context) {
    furi_assert(context);
    WiFiMapApp* app = context;

    if(ev == UartIrqEventRXNE) {
        furi_stream_buffer_send(app->rx_stream, &data, 1, 0);
        furi_thread_flags_set(furi_thread_get_id(app->worker_thread), WorkerEventRx);
    }
}

static void uart_echo_push_to_list(const char data , WiFiMapApp* app) {
    if((data >= ' ' && data <= '~') || (data == '\n' || data == '\r')) {
        write_to_file((char) data,  app->file);
        app->c = data;
        FURI_LOG_D(TAG, "%c", data);
    }
}

static int32_t wifi_map_worker(void* context) {
    furi_assert(context);
    WiFiMapApp* app = context;

    for(bool is_running = true; is_running;) {
        InputEvent event;
        /* Wait for an input event. Input events come from the GUI thread via a callback. */
        const FuriStatus status = furi_message_queue_get(app->event_queue, &event, FuriWaitForever);
        
        size_t length = 0;
        do {
            uint8_t data[64];
            length = furi_stream_buffer_receive(app->rx_stream, data, 64, 0);
            if(length > 0) {
                furi_hal_uart_tx(FuriHalUartIdUSART1, data, length);
                for (size_t i = 0; i < length; i++) {
                    uart_echo_push_to_list(data[i], app);
                }
            }
        } while(length > 0);

        /* This application is only interested in short button presses. */
        if((status != FuriStatusOk) || (event.type != InputTypeShort))
            continue;

        /* When the user presses the "Back" button, break the loop and exit the application. */
        if(event.key == InputKeyBack) 
            is_running = false;
    }

    FURI_LOG_D(TAG, "EXIT");
    return 0;
}

static WiFiMapApp* wifi_map_app_alloc() {
    WiFiMapApp* app = malloc(sizeof(WiFiMapApp));
    app->file = open_file();
    app->rx_stream = furi_stream_buffer_alloc(2048, 1);

    // View
    app->view_port = view_port_alloc();
    view_port_draw_callback_set(app->view_port, wifi_map_view_draw_callback, app);
    view_port_input_callback_set(app->view_port, wifi_map_view_input_callback, app);

    app->event_queue = furi_message_queue_alloc(8, sizeof(InputEvent));

    // Gui
    app->gui = furi_record_open(RECORD_GUI);
    gui_add_view_port(app->gui, app->view_port, GuiLayerFullscreen);

    app->worker_thread = furi_thread_alloc_ex("WifiMapUartWorker", 1024, wifi_map_worker, app);
    furi_thread_start(app->worker_thread);

    // Enable uart listener
    furi_hal_console_disable();
    furi_hal_uart_set_br(FuriHalUartIdUSART1, 115200);
    furi_hal_uart_set_irq_cb(FuriHalUartIdUSART1, uart_echo_on_irq_cb, app);

    return app;
}

static void uart_echo_app_free(WiFiMapApp* app) {
    furi_assert(app);

    furi_hal_console_enable(); // this will also clear IRQ callback so thread is no longer referenced

    furi_thread_flags_set(furi_thread_get_id(app->worker_thread), WorkerEventStop);
    furi_thread_join(app->worker_thread);
    furi_thread_free(app->worker_thread);

    // Free views
    view_port_enabled_set(app->view_port, false);
    gui_remove_view_port(app->gui, app->view_port);
    view_port_free(app->view_port);

    // Close gui record
    furi_record_close(RECORD_GUI);
    furi_record_close(RECORD_NOTIFICATION);
    app->gui = NULL;

    furi_stream_buffer_free(app->rx_stream);

    // closing the file
    close_file(app->file);

    // Free rest
    free(app);
}

int32_t wifi_map_app(void *p){
	UNUSED(p);
	FURI_LOG_I(TAG, "wifi_map_app");
    FURI_LOG_D(TAG, "WTF0");

	WiFiMapApp* app = wifi_map_app_alloc();
    FURI_LOG_D(TAG, "lol0");
    wifi_map_worker(app);
    uart_echo_app_free(app);
	return 0;
}
