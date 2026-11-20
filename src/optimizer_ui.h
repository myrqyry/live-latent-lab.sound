#pragma once

extern "C" void optimizer_tools_cb(void *private_data);
extern "C" void open_model_downloads_cb(void *private_data);
extern "C" void open_docs_cb(void *private_data);
extern "C" void check_updates_cb(void *private_data);
// Call once (from main plugin load) to register the Tools menu items
void init_optimizer_tools_menu();
