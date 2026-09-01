#pragma once
#include <string>

// Filesystem-space guards.
//
// A pull writes hundreds of megabytes to two places - the blob cache and the
// output bundle - and on a small box filling either one is not a normal error:
// a root filesystem at zero bytes free wedges every writer on the machine
// (logs, ubus, the shell) long before anything reports a failure. So the
// converter (a) refuses up front when the image plainly does not fit and (b)
// keeps a watchdog on the filesystem it is writing to and aborts while there is
// still room left to recover in.
namespace space {

struct Info {
	bool ok = false;                 // statvfs() succeeded
	unsigned long long total = 0;    // bytes
	unsigned long long avail = 0;    // bytes available to this user
	unsigned long long dev   = 0;    // st_dev - compare two paths for "same filesystem"
	bool tmpfs = false;              // RAM-backed: filling it eats memory, not disk
	std::string mount;               // the existing path we measured
};

// statvfs() the nearest EXISTING ancestor of `path` (the directory itself need
// not exist yet). ok=false only when even "/" cannot be measured.
Info of(const std::string& path);

// same filesystem?
bool same(const Info& a, const Info& b);

// "1.4 GiB", "512 MiB", "9 KiB"
std::string human(unsigned long long bytes);

// The floor we never write past: 32 MiB, or 1% of the filesystem if that is
// more. Leaving this much keeps the box usable (and the failure recoverable)
// when a pull turns out to be too big after all.
unsigned long long reserve(const Info& i);

// avail - reserve(), clamped at 0: what a pull may actually consume.
unsigned long long usable(const Info& i);

// ---- watchdog ---------------------------------------------------------------
// Arm the guard on the filesystem holding `path` (no-op if it cannot be
// measured). Downloads and layer extraction poll it as they write.
void arm(const std::string& path);
void disarm();

// True once the guarded filesystem has fallen below its floor. Call it from a
// write loop with the number of bytes just written: the statvfs is throttled to
// every 8 MiB of progress or 250 ms, whichever comes first. Both bounds matter -
// time alone loses to a fast filesystem (a whole layer can land inside one
// interval), bytes alone never fire when something ELSE is filling the disk.
bool low(unsigned long long wrote = 0);

// Why the guard tripped, ready to hand to the user ("" when it has not).
const std::string& why();

}
