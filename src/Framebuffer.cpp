#include "Framebuffer.hpp"

#include <ESP32-HUB75-MatrixPanel-I2S-DMA.h>
#include <Mapping.h>

#include "Layout.hpp"

extern MatrixPanel_I2S_DMA* display;
extern VirtualMatrixPanel* virtualDisp;
extern uint8_t color_depth;  // the display's bits per color (main.cpp)

namespace {

// The display library keeps its frame buffers private, and it is a pinned
// fork we don't change. An explicit template instantiation may name private
// members (C++ [temp.explicit]): these two give us pointers to them.
template <typename Tag, typename Tag::type M>
struct Expose {
	friend typename Tag::type get(Tag) { return M; }
};
struct FrameBuffers {
	typedef frameStruct (MatrixPanel_I2S_DMA::*type)[2];
	friend type get(FrameBuffers);
};
struct BackBufferId {
	typedef volatile int MatrixPanel_I2S_DMA::*type;
	friend type get(BackBufferId);
};
template struct Expose<FrameBuffers, &MatrixPanel_I2S_DMA::frame_buffer>;
template struct Expose<BackBufferId, &MatrixPanel_I2S_DMA::back_buffer_id>;

// The ESP32's I2S sends 16-bit words in swapped pairs (the library's
// ESP32_TX_FIFO_POSITION_ADJUST)
inline int fifo(int x) {
	return x ^ 1;
}

// A color code of `planes` bits (what the library keeps of lumConvTab[v],
// its top bits) back to 0..255: the middle of the values that give it
// (0: off, black)
uint8_t decode(uint16_t code, int planes) {
	if (!code)
		return 0;
	const int shift = 16 - planes;
	int lo = 0, hi = 255;
	while (lo < hi) {  // the first value whose code is >= code
		int mid = (lo + hi) / 2;
		if ((lumConvTab[mid] >> shift) < code)
			lo = mid + 1;
		else
			hi = mid;
	}
	int first = lo, last = lo;
	while (last < 255 && (lumConvTab[last + 1] >> shift) == code)
		last++;
	return (first + last + 1) / 2;
}

}  // namespace

void scroll_display(int dx, int dy, bool wrap) {
	if (!display || !virtualDisp)
		return;
	const HUB75_I2S_CFG& cfg = display->getCfg();
	if (!cfg.double_buff)
		return;  // the source and the destination would be one buffer
	frameStruct* fbs = display->*get(FrameBuffers());
	int back = display->*get(BackBufferId());
	frameStruct& dst = fbs[back];
	frameStruct& src = fbs[back ^ 1];  // what is on screen
	const int half = cfg.mx_height / 2;  // rows refreshed in parallel: the upper and lower halves share a word
	const int planes = color_depth;
	const int w = matrix_w, h = matrix_h;

	// as wired (one picture = the chain): row by row, plane by plane
	if (layout->rows == 1 && layout->cols == 1) {
		const uint16_t RGB = 7;
		for (int y = 0; y < h; y++) {
			int sy = y - dy;
			if (wrap)
				sy = ((sy % h) + h) % h;
			const bool row_in = sy >= 0 && sy < h;
			rowBitStruct* drow = dst.rowBits[y % half].get();
			rowBitStruct* srow = row_in ? src.rowBits[sy % half].get() : nullptr;
			const int dshift = y >= half ? 3 : 0, sshift = sy >= half ? 3 : 0;
			const uint16_t keep = ~(RGB << dshift);
			for (int p = 0; p < planes; p++) {
				uint16_t* d = drow->data + p * drow->width;
				if (!row_in) {
					for (int x = 0; x < w; x++)
						d[x] &= keep;
					continue;
				}
				const uint16_t* sp = srow->data + p * srow->width;
				for (int x = 0; x < w; x++) {
					int sx = x - dx;
					if (wrap)
						sx = ((sx % w) + w) % w;
					uint16_t rgb = sx >= 0 && sx < w ? (sp[fifo(sx)] >> sshift) & RGB : 0;
					uint16_t& word = d[fifo(x)];
					word = (word & keep) | (rgb << dshift);
				}
			}
		}
		return;
	}

	// a panel map (the cross…): each pixel where the map puts it
	for (int y = 0; y < h; y++) {
		for (int x = 0; x < w; x++) {
			int16_t px, py;
			if (!virtualDisp->physical(x, y, px, py))
				continue;
			int sx = x - dx, sy = y - dy;
			if (wrap) {
				sx = ((sx % w) + w) % w;
				sy = ((sy % h) + h) % h;
			}
			int16_t qx = 0, qy = 0;
			bool from = sx >= 0 && sx < w && sy >= 0 && sy < h && virtualDisp->physical(sx, sy, qx, qy);
			const int dshift = py >= half ? 3 : 0, sshift = qy >= half ? 3 : 0;
			rowBitStruct* drow = dst.rowBits[py % half].get();
			rowBitStruct* srow = from ? src.rowBits[qy % half].get() : nullptr;
			const size_t dw = drow->width;
			const int di = fifo(px), si = fifo(qx);
			for (int p = 0; p < planes; p++) {
				uint16_t& word = drow->data[p * dw + di];
				uint16_t rgb = from ? (srow->data[p * srow->width + si] >> sshift) & 7 : 0;
				word = (word & ~(7 << dshift)) | (rgb << dshift);
			}
		}
	}
}

bool get_pixel(int x, int y, uint8_t& r, uint8_t& g, uint8_t& b) {
	int16_t px, py;
	if (!display || !virtualDisp || !virtualDisp->physical(x, y, px, py))
		return false;
	const HUB75_I2S_CFG& cfg = display->getCfg();
	frameStruct* fbs = display->*get(FrameBuffers());
	frameStruct& fb = fbs[cfg.double_buff ? display->*get(BackBufferId()) : 0];
	const int half = cfg.mx_height / 2, shift = py >= half ? 3 : 0;
	const int planes = color_depth;
	rowBitStruct* row = fb.rowBits[py % half].get();
	uint16_t cr = 0, cg = 0, cb = 0;
	for (int p = 0; p < planes; p++) {  // plane p: bit p of the code
		uint16_t rgb = (row->data[p * row->width + fifo(px)] >> shift) & 7;
		cr |= (rgb & 1) << p;
		cg |= ((rgb >> 1) & 1) << p;
		cb |= ((rgb >> 2) & 1) << p;
	}
	r = decode(cr, planes);
	g = decode(cg, planes);
	b = decode(cb, planes);
	return true;
}
