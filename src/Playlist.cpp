#include "Playlist.hpp"

#include <SpectreProtocol.h>
#include <esp_random.h>

#include <vector>

#include "Gif.hpp"
#include "Log.hpp"
#include "Lua.hpp"
#include "Protocol.hpp"
#include "Storage.hpp"

namespace {

constexpr uint32_t DEFAULT_MS = 10000;
constexpr size_t MAX_FILE = 16 * 1024;
constexpr size_t MAX_ITEMS = 256;
constexpr uint32_t LUA_GRACE_MS = 500;  // before "the script ended by itself" counts

struct Item {
	String path;     // absolute
	uint32_t ms;     // 0: not given
	uint16_t loops;  // 0: not given
};

std::vector<Item> items;
std::vector<uint16_t> order;  // play order of this round (shuffle)
bool shuffle = false, once = false;
Item defaults{"", DEFAULT_MS, 0};
String list_path;
bool running = false;

size_t pos = 0;            // in order
uint32_t started = 0;      // millis() the item started
uint32_t deadline_loops;   // GIF loops done when its time was up (finish that loop)
bool deadline_hit = false;
uint32_t ms, loops;        // the item's, defaults applied
bool is_gif = false, is_lua = false;
size_t failures = 0;       // in a row: a whole round failing stops the playlist

bool has_ext(const String& p, const char* ext) {
	return p.length() >= strlen(ext) && p.substring(p.length() - strlen(ext)).equalsIgnoreCase(ext);
}

// "20s", "1m30s", "500ms", "1h", "1.5s": milliseconds, 0 if not a time
uint32_t parse_time(const String& t) {
	uint32_t total = 0;
	size_t i = 0, n = t.length();
	if (!n)
		return 0;
	while (i < n) {
		size_t start = i;
		while (i < n && (isdigit(t[i]) || t[i] == '.'))
			i++;
		if (i == start)
			return 0;
		float v = t.substring(start, i).toFloat();
		String unit;
		while (i < n && isalpha(t[i]))
			unit += (char)tolower(t[i++]);
		uint32_t mul = unit == "ms" ? 1 : unit == "s" ? 1000 : unit == "m" ? 60000 : unit == "h" ? 3600000 : 0;
		if (!mul)
			return 0;
		total += (uint32_t)(v * mul);
	}
	return total;
}

// "3x": 3, 0 if not a loop count
uint16_t parse_loops(const String& t) {
	if (t.length() < 2 || tolower(t[t.length() - 1]) != 'x')
		return 0;
	for (size_t i = 0; i + 1 < t.length(); i++)
		if (!isdigit(t[i]))
			return 0;
	return (uint16_t)t.substring(0, t.length() - 1).toInt();
}

// Options at the end of a line ("gif/a b.gif 3x 20s"): applied to `item`,
// the rest (the path, spaces kept) returned.
String take_options(String line, Item& item) {
	for (;;) {
		line.trim();
		int sp = line.lastIndexOf(' ');
		int tab = line.lastIndexOf('\t');
		int cut = max(sp, tab);
		String tok = line.substring(cut + 1);  // cut -1: the whole line
		if (uint32_t t = parse_time(tok))
			item.ms = t;
		else if (uint16_t l = parse_loops(tok))
			item.loops = l;
		else
			return line;
		line = cut < 0 ? String() : line.substring(0, cut);
	}
}

bool parse(const String& text) {
	items.clear();
	shuffle = once = false;
	defaults = Item{"", DEFAULT_MS, 0};
	int line_no = 0;
	for (int start = 0; start < (int)text.length() && items.size() < MAX_ITEMS;) {
		int end = text.indexOf('\n', start);
		if (end < 0)
			end = text.length();
		String line = text.substring(start, end);
		start = end + 1;
		line_no++;
		line.trim();  // also the \r of CRLF files
		if (!line.length() || line[0] == '#')
			continue;
		if (line.equalsIgnoreCase("shuffle")) {
			shuffle = true;
			continue;
		}
		if (line.equalsIgnoreCase("once")) {
			once = true;
			continue;
		}
		if (line.startsWith("default ") || line.startsWith("default\t")) {
			Item d{"", 0, 0};
			String rest = take_options(line.substring(8), d);
			rest.trim();
			if (rest.length() || (!d.ms && !d.loops))
				Log.printf("playlist line %d: default: a time and / or loops (\"default 15s\")\n", line_no);
			else
				defaults = d;
			continue;
		}
		Item item{"", 0, 0};
		String p = take_options(line, item);
		if (!has_ext(p, ".gif") && !has_ext(p, ".png") && !has_ext(p, ".lua")) {
			Log.printf("playlist line %d: not a .gif / .png / .lua: %s\n", line_no, p.c_str());
			continue;
		}
		item.path = p[0] == '/' ? p : board_dir() + "/" + p;
		items.push_back(item);
	}
	return !items.empty();
}

void new_round() {
	order.resize(items.size());
	for (size_t i = 0; i < order.size(); i++)
		order[i] = i;
	if (shuffle)
		for (size_t i = order.size() - 1; i > 0; i--)
			std::swap(order[i], order[esp_random() % (i + 1)]);
	pos = 0;
}

// Start order[pos]: true if it plays
bool play_current() {
	const Item& it = items[order[pos]];
	is_gif = has_ext(it.path, ".gif");
	is_lua = has_ext(it.path, ".lua");
	ms = it.ms;
	loops = it.loops;
	if (!ms && !loops) {
		ms = defaults.ms;
		loops = defaults.loops;
	}
	if (!is_gif && !ms)  // loops alone: for GIFs only
		ms = defaults.ms ? defaults.ms : DEFAULT_MS;
	deadline_hit = false;
	started = millis();
	if (uint16_t err = play_media(it.path.c_str())) {
		Log.printf("playlist: can't play %s (error %u), next\n", it.path.c_str(), err);
		return false;
	}
	playing_changed(list_path.c_str(), it.path.c_str());
	return true;
}

// Move by `delta` and start the item there (skipping the ones that fail).
void advance(int delta) {
	for (;;) {
		int next = (int)pos + delta;
		if (next >= (int)order.size()) {
			if (once) {
				Log.println("playlist: done");
				Playlist::stop();
				playing_changed("", "");
				return;
			}
			new_round();
		} else if (next < 0) {
			pos = order.size() - 1;
		} else {
			pos = next;
		}
		if (play_current()) {
			failures = 0;
			return;
		}
		if (++failures >= items.size()) {
			Log.line(LogLevel::Error, "playlist: none of its files plays, stopped");
			Playlist::stop();
			playing_changed("", "");
			return;
		}
		delta = delta < 0 ? -1 : 1;
	}
}

bool item_done() {
	uint32_t elapsed = millis() - started;
	if (is_gif) {
		if (!SpectreGif::playing())
			return true;  // could not open it / stopped
		uint32_t done = SpectreGif::loops();
		if (loops && done >= loops)
			return true;
		if (ms && elapsed >= ms) {
			if (!deadline_hit) {  // finish the loop it is in
				deadline_hit = true;
				deadline_loops = done;
			}
			return done > deadline_loops;
		}
		return false;
	}
	if (is_lua && elapsed > LUA_GRACE_MS && !Lua::running())
		return true;  // the script ended by itself
	return elapsed >= ms;
}

}  // namespace

namespace Playlist {

uint16_t start(const char* path) {
	File f = storage->open(path);
	if (!f || f.isDirectory())
		return spectre::error::NotFound;
	if (f.size() > MAX_FILE) {
		f.close();
		return spectre::error::BadArgs;
	}
	String text = f.readString();
	f.close();
	if (!parse(text)) {
		Log.printf("playlist %s: nothing to play\n", path);
		return spectre::error::BadArgs;
	}
	list_path = path;
	running = true;
	failures = 0;
	new_round();
	Log.printf("playlist %s: %u items%s%s\n", path, (unsigned)items.size(), shuffle ? ", shuffle" : "", once ? ", once" : "");
	if (!play_current())
		advance(1);
	return running ? 0 : spectre::error::NotFound;
}

void stop() {
	running = false;
}

bool active() {
	return running;
}

const char* path() {
	return list_path.c_str();
}

bool skip(int delta) {
	if (!running)
		return false;
	failures = 0;
	advance(delta < 0 ? -1 : 1);
	return true;
}

void replay() {
	if (running && !play_current())
		advance(1);
}

void loop() {
	if (running && item_done())
		advance(1);
}

}  // namespace Playlist
