/*
  Mono synth, Moog-style 3-oscillator unison, legato note-stack, ADSR. Mozzi + MIDI.
  Arduino Uno R3.

  Audio out : pin 9 (Mozzi standard PWM mode)
  MIDI in   : RX (pin 0), via MIDI Library
  A0        : voice 2 detune, -1 to +1 octave around the played note
  A1        : voice 3 detune, -1 to +1 octave around the played note
  A2        : attack time
  A3        : release time (how long the note tails off after note-off)
  A4        : lowpass filter cutoff

  Legato note-stack: holding a note and playing another just moves the pitch,
  no envelope retrigger. Releasing the top note falls back to whatever's
  still held underneath. The envelope only attacks on 0->1 held notes, and
  only releases on 1->0 (last finger off).
*/

// Overall output trim. 1.0 = unity, lower it if the signal is too hot.
float masterVolume = 0.7f;
int16_t masterVolumeQ8; // precomputed fixed-point version, set from masterVolume in setup()

#define MOZZI_CONTROL_RATE 128
#include <Mozzi.h>
#include <Oscil.h>
#include <ADSR.h>
#include <tables/saw2048_int8.h>

#include <MIDI.h>
MIDI_CREATE_DEFAULT_INSTANCE();

Oscil<SAW2048_NUM_CELLS, MOZZI_AUDIO_RATE> osc1(SAW2048_DATA);
Oscil<SAW2048_NUM_CELLS, MOZZI_AUDIO_RATE> osc2(SAW2048_DATA);
Oscil<SAW2048_NUM_CELLS, MOZZI_AUDIO_RATE> osc3(SAW2048_DATA);
ADSR<MOZZI_CONTROL_RATE, MOZZI_AUDIO_RATE> env;

// Simple one-pole lowpass, 16-bit state (avoids 8-bit feedback quantization
// grain) and pure integer math (no floats in the audio-rate hot path).
int16_t filterState = 0;
byte filterCutoff = 255; // 0..255, higher = brighter/more open

#define STACK_SIZE 8
byte noteStack[STACK_SIZE];
byte stackSize = 0;
float baseFreq = 440.0f;
unsigned long ledOffAt = 0;

void setCurrentNote(byte note) {
  baseFreq = 440.0f * pow(2.0f, (note - 69) / 12.0f);
  osc1.setFreq(baseFreq);
}

void noteOn(byte channel, byte note, byte velocity) {
  bool wasEmpty = (stackSize == 0); // snapshot BEFORE any dedup bookkeeping below

  // if already held (e.g. duplicate note-on), drop it so it moves to the top
  for (byte i = 0; i < stackSize; i++) {
    if (noteStack[i] == note) {
      for (byte j = i; j < stackSize - 1; j++) noteStack[j] = noteStack[j + 1];
      stackSize--;
      break;
    }
  }
  if (stackSize >= STACK_SIZE) return; // stack full, drop the note

  noteStack[stackSize++] = note;
  setCurrentNote(note);
  if (wasEmpty) env.noteOn(); // only retrigger the envelope on a fresh phrase

  digitalWrite(13, HIGH);
  ledOffAt = millis() + 30;
}

void noteOff(byte channel, byte note, byte velocity) {
  for (byte i = 0; i < stackSize; i++) {
    if (noteStack[i] == note) {
      for (byte j = i; j < stackSize - 1; j++) noteStack[j] = noteStack[j + 1];
      stackSize--;
      if (stackSize == 0) {
        env.noteOff(); // last finger off, release
      } else {
        setCurrentNote(noteStack[stackSize - 1]); // fall back to the note underneath
      }
      return;
    }
  }
}

void setup() {
  pinMode(13, OUTPUT);

  MIDI.setHandleNoteOn(noteOn);
  MIDI.setHandleNoteOff(noteOff);
  MIDI.begin(2); // channel 2 only
  MIDI.turnThruOff(); // no pass-through needed, and it corrupts the RX line otherwise

  masterVolumeQ8 = (int16_t)(masterVolume * 256);

  env.setADLevels(255, 200);
  env.setTimes(10, 120, 20000, 250); // standard ADSR, sustain held open for performance

  startMozzi();
}

void updateControl() {
  MIDI.read();
  if (millis() > ledOffAt) digitalWrite(13, LOW);
  env.update();

  int cents2 = map(mozziAnalogRead<10>(A0), 0, 1023, -1200, 1200);
  int cents3 = map(mozziAnalogRead<10>(A1), 0, 1023, -1200, 1200);
  osc2.setFreq((float)(baseFreq * pow(2.0f, cents2 / 1200.0f)));
  osc3.setFreq((float)(baseFreq * pow(2.0f, cents3 / 1200.0f)));

  unsigned int attackMs = map(mozziAnalogRead<10>(A2), 0, 1023, 1, 500);
  unsigned int releaseMs = map(mozziAnalogRead<10>(A3), 0, 1023, 10, 1000);
  env.setAttackTime(attackMs);
  env.setReleaseTime(releaseMs);

  filterCutoff = mozziAnalogRead<8>(A4);
}

AudioOutput updateAudio() {
  if (!env.playing()) {
    filterState = 0; // nothing sounding, don't let old state bleed into the next note
    return MonoOutput::from8Bit(0);
  }

  int8_t mixed = (int8_t)((osc1.next() + osc2.next() + osc3.next()) / 3);
  int16_t enveloped = ((int16_t)mixed * (int16_t)env.next()) >> 8;
  filterState += ((int32_t)(enveloped - filterState) * filterCutoff) >> 8;
  int16_t out = (filterState * masterVolumeQ8) >> 8;
  return MonoOutput::from8Bit((int8_t)out);
}

void loop() {
  audioHook();
}
