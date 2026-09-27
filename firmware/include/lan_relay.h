#pragma once
namespace LanRelay {
// Called only from the network task; immutable credentials cross a mailbox.
void configure(const char* api, const char* token, bool paired);
}
