#pragma once
// Standard Open / Save As dialog for apps, shared from /System/Libraries.
//
//   static const file_dialog::Filter kFilters[] = {{"Images", "png;jpg"}, {"All files", ""}};
//   file_dialog::Options o;
//   o.filters = kFilters;
//   o.filter_count = 2;
//   file_dialog::open(this, o, [this](const char* path) { load(path); });
//
// The callback runs only when the user confirms, with an absolute path. Call
// file_dialog::close_for(this) from the owner window's destructor.
#include "ui/func.h"

class Window;

namespace file_dialog {

struct Filter {
    const char* label;        // "Images"
    const char* extensions;   // "png;jpg;jpeg", lowercase, or "" for every file
};

struct Options {
    const char* title = nullptr;              // default "Open" / "Save As"
    const char* directory = nullptr;          // default the user's Documents
    const char* file_name = "";               // pre-filled name
    const Filter* filters = nullptr;
    int filter_count = 0;
    const char* default_extension = nullptr;  // save: appended when the name has none
};

using Done = Func<void(const char* path)>;

void open(Window* owner, const Options& options, Done done);
void save(Window* owner, const Options& options, Done done);
void close_for(Window* owner);                // dismiss without calling back

}  // namespace file_dialog
