namespace SpectreGif {
  uint8_t init();
  void play(const char*);
  // Stop and wait until the GIF task has released the arena.
  // Returns false if it did not within 500 ms.
  bool stop();
  bool isPlaying(const char*);
  // A GIF plays (opened, not stopped; false when it could not be opened).
  bool playing();
  // Loops played to the end since play() (playlists count them).
  uint32_t loops();
}