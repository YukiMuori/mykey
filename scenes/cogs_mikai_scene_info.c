#include "../cogs_mikai.h"
#include <machine/endian.h>

typedef enum {
    InfoSceneEventMenu = 1,      // Left key: back to main menu (card stays loaded)
    InfoSceneEventAddCredit = 2, // Right key: open Add Credit
} InfoSceneEvent;

// Called when the "Menu" (left key) or "Add Credit" (right key) button is pressed
static void cogs_mikai_scene_info_button_callback(
    GuiButtonType result,
    InputType type,
    void* context) {
    COGSMyKaiApp* app = context;

    if(type == InputTypeShort) {
        if(result == GuiButtonTypeLeft) {
            view_dispatcher_send_custom_event(app->view_dispatcher, InfoSceneEventMenu);
        } else if(result == GuiButtonTypeRight) {
            view_dispatcher_send_custom_event(app->view_dispatcher, InfoSceneEventAddCredit);
        }
    }
}

void cogs_mikai_scene_info_on_enter(void* context) {
    COGSMyKaiApp* app = context;
    FuriString* text = app->text_box_store;

    furi_string_reset(text);

    if(!app->mykey.is_loaded) {
        furi_string_cat(text, "No Card Loaded\n\nPlease read a card first.");
    } else {

        furi_string_cat_printf(
            text, "Serial: %08lX\n", (unsigned long)app->mykey.eeprom[0x07]);

        // vendor ID - calculated from blocks 0x18 and 0x19
        uint32_t block18 = app->mykey.eeprom[0x18];
        uint32_t block19 = app->mykey.eeprom[0x19];
        mykey_encode_decode_block(&block18);
        mykey_encode_decode_block(&block19);
        uint64_t vendor = (((uint64_t)block18 << 16) | (block19 & 0x0000FFFF)) + 1;

        furi_string_cat_printf(text, "Vendor: %llX\n", vendor);

        // current credit
        furi_string_cat_printf(
            text,
            "Credit: %u.%02u EUR\n",
            app->mykey.current_credit / 100,
            app->mykey.current_credit % 100);

        // card status
        furi_string_cat_printf(text, "Status: %s\n", app->mykey.is_reset ? "Reset" : "Active");

        // operation count (block 0x12, lower 24 bits)
        uint32_t op_count = app->mykey.eeprom[0x12] & 0x00FFFFFF;
        furi_string_cat_printf(text, "Operations: %lu\n", (unsigned long)op_count);

        // UID
        furi_string_cat_printf(
            text,
            "UID: %08lX%08lX\n",
            (unsigned long)(app->mykey.uid >> 32),
            (unsigned long)(app->mykey.uid & 0xFFFFFFFF));

        // parse and display full transaction history
        uint32_t block3C = app->mykey.eeprom[0x3C];
        if(block3C != 0xFFFFFFFF) {
            block3C ^= app->mykey.eeprom[0x07];
            uint32_t starting_offset =
                ((block3C & 0x30000000) >> 28) | ((block3C & 0x00100000) >> 18);

            if(starting_offset < 8) {
                // first, find how many transactions exist by going forward from starting_offset
                int num_transactions = 0;
                for(int i = 0; i < 8; i++) {
                    uint32_t txn_block = app->mykey.eeprom[0x34 + ((starting_offset + i) % 8)];
                    if(txn_block == 0xFFFFFFFF) {
                        break;
                    }
                    num_transactions++;
                }

                if(num_transactions > 0) {
                    furi_string_cat(text, "\n=== Transaction History ===\n");
                    furi_string_cat(text, "(Newest first)\n\n");

                    // display transactions in reverse order (newest first)
                    for(int i = num_transactions - 1; i >= 0; i--) {
                        uint32_t txn_block = app->mykey.eeprom[0x34 + ((starting_offset + i) % 8)];

                        // extract transaction fields directly from big-endian block
                        uint8_t day = txn_block >> 27;
                        uint8_t month = (txn_block >> 23) & 0xF;
                        uint16_t year = 2000 + ((txn_block >> 16) & 0x7F);
                        uint16_t credit = txn_block & 0xFFFF;

                        furi_string_cat_printf(
                            text,
                            "%d. %02d/%02d/%04d - %d.%02d EUR\n",
                            num_transactions - i,
                            day,
                            month,
                            year,
                            credit / 100,
                            credit % 100);
                    }
                } else {
                    furi_string_cat(text, "\nNo transaction history\n");
                }
            } else {
                furi_string_cat(text, "\nTransaction history:\n");
                furi_string_cat(text, "Invalid offset\n");
            }
        } else {
            furi_string_cat(text, "\nTransaction history:\n");
            furi_string_cat(text, "Not available\n");
        }
    }

    // Show the info as a scrollable text. With a card loaded, two buttons
    // appear at the bottom: "Menu" (left key) returns to the main menu
    // with the card still loaded (Set Credit, Reset, Save, Debug, ...),
    // "Add Credit" (right key) goes straight to the amount selection.
    Widget* widget = app->widget;
    widget_reset(widget);

    if(app->mykey.is_loaded) {
        widget_add_text_scroll_element(widget, 0, 0, 128, 50, furi_string_get_cstr(text));
        widget_add_button_element(
            widget,
            GuiButtonTypeLeft,
            "Menu",
            cogs_mikai_scene_info_button_callback,
            app);
        widget_add_button_element(
            widget,
            GuiButtonTypeRight,
            "Add Credit",
            cogs_mikai_scene_info_button_callback,
            app);
    } else {
        widget_add_text_scroll_element(widget, 0, 0, 128, 64, furi_string_get_cstr(text));
    }

    view_dispatcher_switch_to_view(app->view_dispatcher, COGSMyKaiViewWidget);
}

bool cogs_mikai_scene_info_on_event(void* context, SceneManagerEvent event) {
    COGSMyKaiApp* app = context;
    bool consumed = false;

    if(event.type == SceneManagerEventTypeCustom) {
        if(event.event == InfoSceneEventAddCredit) {
            if(app->mykey.is_loaded) {
                scene_manager_next_scene(app->scene_manager, COGSMyKaiSceneAddCredit);
                consumed = true;
            }
        } else if(event.event == InfoSceneEventMenu) {
            // Back to the main menu, card stays loaded: all functions
            // (Set Credit, Reset, Save, Debug, ...) remain available
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, COGSMyKaiSceneStart);
            consumed = true;
        }
    } else if(event.type == SceneManagerEventTypeBack) {
        if(app->mykey.is_loaded) {
            // Same as the left button: skip the Read scene still in the
            // stack (it would restart the read) and go to the menu
            scene_manager_search_and_switch_to_previous_scene(
                app->scene_manager, COGSMyKaiSceneStart);
        } else {
            scene_manager_previous_scene(app->scene_manager);
        }
        consumed = true;
    }

    return consumed;
}

void cogs_mikai_scene_info_on_exit(void* context) {
    COGSMyKaiApp* app = context;
    widget_reset(app->widget);
    furi_string_reset(app->text_box_store);
}
