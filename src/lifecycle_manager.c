/* In kernel_main.c or lifecycle_manager.c */

static void on_storage_event(storage_state_t state, const char* detail) {
    switch (state) {
        case STORAGE_STATE_EJECTED:
            /* Immediately stop current app if running */
            if (g_current_app_ctx) {
                ESP_LOGW("LIFECYCLE", "Card ejected! Force-unloading app");
                app_loader_unload(g_current_app_ctx);
                g_current_app_ctx = NULL;
            }
            /* Show "Insert SD Card" overlay on display */
            svc_display_show_system_overlay(OVERLAY_NO_CARD);
            break;

        case STORAGE_STATE_UNFORMATTED:
            /* Show "New card detected. Download apps?" dialog */
            svc_display_show_system_dialog(
                "New Storage", 
                "This card is empty. Download default apps from server?",
                on_provision_confirm  /* Callback if user says yes */
            );
            break;

        case STORAGE_STATE_PROVISIONING:
            /* Show progress bar overlay */
            svc_display_show_progress_overlay("Setting up...", 0);
            break;

        case STORAGE_STATE_READY:
            /* Dismiss overlays, load launcher or resume last app */
            svc_display_dismiss_overlay();
            if (!g_current_app_ctx) {
                load_and_run_app("launcher");
            }
            break;

        case STORAGE_STATE_ERROR:
            svc_display_show_system_overlay(OVERLAY_SD_ERROR);
            break;
    }
}

/* Called during kernel init, AFTER svc_storage_init() */
void lifecycle_init(void) {
    svc_storage_on_event(on_storage_event);
    
    /* Handle initial state at boot */
    storage_state_t initial = svc_storage_get_state();
    if (initial == STORAGE_STATE_READY) {
        load_and_run_app("clock"); /* Default startup app */
    } else {
        on_storage_event(initial, "Boot state");
    }
}
