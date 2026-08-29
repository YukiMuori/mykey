#include "../cogs_mikai.h"
#include <storage/storage.h>
#include <furi_hal_rtc.h>

typedef enum {
    ExportHistoryEventClose = 1, // Result popup dismissed
} ExportHistoryEvent;

static void cogs_mikai_scene_export_history_popup_callback(void* context) {
    COGSMyKaiApp* app = context;
    view_dispatcher_send_custom_event(app->view_dispatcher, ExportHistoryEventClose);
}

void cogs_mikai_scene_export_history_on_enter(void* context) {
    COGSMyKaiApp* app = context;
    Popup* popup = app->popup;

    if(!app->mykey.is_loaded) {
        popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
        popup_set_text(popup, "No card loaded\nRead a card first", 64, 25, AlignCenter, AlignTop);
        popup_set_callback(popup, cogs_mikai_scene_export_history_popup_callback);
        popup_set_context(popup, app);
        popup_set_timeout(popup, 2000);
        popup_enable_timeout(popup);
        view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);
        return;
    }

    const char* export_dir = "/ext/apps_data/cogs_mikai";
    Storage* storage = furi_record_open(RECORD_STORAGE);
    storage_simply_mkdir(storage, export_dir);

    DateTime datetime;
    furi_hal_rtc_get_datetime(&datetime);

    FuriString* file_path = furi_string_alloc();
    furi_string_printf(
        file_path,
        "%s/history_%016llX_%04d%02d%02d_%02d%02d%02d.csv",
        export_dir,
        (unsigned long long)app->mykey.uid,
        datetime.year,
        datetime.month,
        datetime.day,
        datetime.hour,
        datetime.minute,
        datetime.second);

    File* file = storage_file_alloc(storage);
    bool success = false;

    if(storage_file_open(file, furi_string_get_cstr(file_path), FSAM_WRITE, FSOM_CREATE_ALWAYS)) {
        FuriString* line = furi_string_alloc();

        // Header with card identity
        furi_string_printf(line, "COGES MyKey history export\n");
        storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));

        furi_string_printf(
            line,
            "UID,%016llX\n",
            (unsigned long long)app->mykey.uid);
        storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));

        furi_string_printf(
            line,
            "Serial,%08lX\n",
            (unsigned long)app->mykey.eeprom[0x07]);
        storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));

        furi_string_printf(
            line,
            "Credit,%u.%02u\n",
            app->mykey.current_credit / 100,
            app->mykey.current_credit % 100);
        storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));

        furi_string_printf(
            line,
            "Exported,%04d-%02d-%02d %02d:%02d:%02d\n",
            datetime.year,
            datetime.month,
            datetime.day,
            datetime.hour,
            datetime.minute,
            datetime.second);
        storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));

        furi_string_printf(line, "\nTx #,Date,Amount\n");
        storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));

        // Transaction history (newest first, same parsing as View Info)
        uint32_t block3C = app->mykey.eeprom[0x3C];
        if(block3C != 0xFFFFFFFF) {
            block3C ^= app->mykey.eeprom[0x07];
            uint32_t starting_offset =
                ((block3C & 0x30000000) >> 28) | ((block3C & 0x00100000) >> 18);

            if(starting_offset < 8) {
                int num_transactions = 0;
                for(int i = 0; i < 8; i++) {
                    uint32_t txn_block = app->mykey.eeprom[0x34 + ((starting_offset + i) % 8)];
                    if(txn_block == 0xFFFFFFFF) {
                        break;
                    }
                    num_transactions++;
                }

                for(int i = num_transactions - 1; i >= 0; i--) {
                    uint32_t txn_block = app->mykey.eeprom[0x34 + ((starting_offset + i) % 8)];

                    uint8_t day = txn_block >> 27;
                    uint8_t month = (txn_block >> 23) & 0xF;
                    uint16_t year = 2000 + ((txn_block >> 16) & 0x7F);
                    uint16_t credit = txn_block & 0xFFFF;

                    furi_string_printf(
                        line,
                        "%d,%02d/%02d/%04d,%d.%02d\n",
                        num_transactions - i,
                        day,
                        month,
                        year,
                        credit / 100,
                        credit % 100);
                    storage_file_write(file, furi_string_get_cstr(line), furi_string_size(line));
                }
            }
        }

        furi_string_free(line);
        storage_file_close(file);
        success = true;
    }

    storage_file_free(file);
    furi_record_close(RECORD_STORAGE);

    if(success) {
        FURI_LOG_I(TAG, "History exported: %s", furi_string_get_cstr(file_path));
        popup_set_header(popup, "Success!", 64, 10, AlignCenter, AlignTop);
        popup_set_text(
            popup,
            "History exported\napps_data/cogs_mikai/",
            64,
            25,
            AlignCenter,
            AlignTop);
        notification_message(app->notifications, &sequence_success);
        popup_set_timeout(popup, 1500);
    } else {
        FURI_LOG_E(TAG, "Failed to export history: %s", furi_string_get_cstr(file_path));
        popup_set_header(popup, "Error", 64, 10, AlignCenter, AlignTop);
        popup_set_text(popup, "Export failed\nCheck SD card", 64, 25, AlignCenter, AlignTop);
        notification_message(app->notifications, &sequence_error);
        popup_set_timeout(popup, 2000);
    }

    popup_set_callback(popup, cogs_mikai_scene_export_history_popup_callback);
    popup_set_context(popup, app);
    popup_enable_timeout(popup);
    view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewPopup);

    furi_string_free(file_path);
}

bool cogs_mikai_scene_export_history_on_event(void* context, SceneManagerEvent event) {
    COGSMyKaiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom && event.event == ExportHistoryEventClose) {
        // Back to the main menu
        scene_manager_search_and_switch_to_previous_scene(
            app->scene_manager, COGSMyKaiSceneStart);
        consumed = true;
    }

    return consumed;
}

void cogs_mikai_scene_export_history_on_exit(void* context) {
    COGSMyKaiApp* app = context;
    popup_reset(app->popup);
}
