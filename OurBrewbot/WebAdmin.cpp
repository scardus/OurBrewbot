/*
 * Copyright 2026 Sean Cardus
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

/*
 * WebAdmin.cpp — Admin configuration page (PROGMEM HTML)
 */

#include "WebAPI.h"
#include "WebUI.h"
#include "WebUIAssets.h"

// ============================================================
// ADMIN PAGE — generated from web/pages/admin.html
// Sent as gzip directly from flash without a page-sized RAM copy
// ============================================================


// ============================================================
// ADMIN PAGE HANDLER
// ============================================================

void handleAdmin(ESP8266WebServer& server) {
  sendWebPage(server, ADMIN_PAGE, sizeof(ADMIN_PAGE));
}

