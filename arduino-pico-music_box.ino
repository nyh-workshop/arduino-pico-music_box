// Sophie's Music Box: https://www.craigandheather.net/celemusicbox.html
// Modified MajicDesigns' Midi Parser for LittleFS from https://github.com/nyh-workshop/MD_MIDIFile

#include <stdint.h>

#include <I2S.h>
#include <pio_i2s.pio.h>

#include <MD_MIDIFile.h>

#include "wavetable.h"

// Default GPIO pin numbers
#define DEFAULT_pBCLK 31
#define DEFAULT_pWS (DEFAULT_pBCLK+1)
#define DEFAULT_pDOUT 34
#define SAMPLE_RATE 39062

#define SAMPLES_PER_BUFFER 256

I2S i2s;
MD_MIDIFile SMF;

static enum { S_IDLE, S_PLAYING } state = S_IDLE;

// Number of simultaneous notes that can sound
#define NUMBER_OF_GENERATORS 8 
#define SUSTAIN_LENGTH 128
#define POT 8

// Length of wave table data
const uint16_t WAVETABLE_LENGTH = sizeof(waveTableData);

// Basic Struct for Sound Generator Data:
struct SoundGenerator
{
  uint16_t m;
  uint32_t phaseAccumulator;
  uint16_t envelopeIndex;
  bool isPlaying;
  uint8_t isPlayingMidiNote;
};

SoundGenerator sndg[NUMBER_OF_GENERATORS];

void midiCallback(midi_event *pev)
// Called by the MIDIFile library when a file event needs to be processed
// thru the midi communications interface.
// This callback is set up in the setup() function.
{
  // Define constants for MIDI channel voice message IDs
  const uint8_t NOTE_OFF = 0x80;  // note on
  const uint8_t NOTE_ON = 0x90;   // note off. NOTE_ON with velocity 0 is same as NOTE_OFF

  switch (pev->data[0]) {
    case NOTE_OFF:  // [1]=note no, [2]=velocity
      midiNoteOff(pev->data[1]);
      Serial.printf("NOTE_OFF ch: %d\n", pev->data[1]);
      break;

    case NOTE_ON:  // [1]=note_no, [2]=velocity
      // Note ON with velocity 0 is the same as off
      // convert scale_table to midi number!
      midiNoteOn(pev->data[1]);
      // midiNoteOn(pev->data[1], pev->channel);
      Serial.printf("NOTE_ON trk: %d, ch: %d, no: %d\n", pev->track, pev->channel, pev->data[1]);
      break;

    default:
      break;
  }
}

void midiNoteOn(uint8_t num) {
  for (uint8_t i = 0; i < NUMBER_OF_GENERATORS; i++) {
    if(!sndg[i].isPlaying) {
      sndg[i].isPlaying = true;
      sndg[i].isPlayingMidiNote = num;
      
      sndg[i].m = midiPitchData[num];
      sndg[i].phaseAccumulator = 0;
      sndg[i].envelopeIndex = 0;
      break;
    }
  }
}

void midiNoteOff(uint8_t num) {
  for (uint8_t i = 0; i < NUMBER_OF_GENERATORS; i++) {
    if(sndg[i].isPlaying && (sndg[i].isPlayingMidiNote == num))
    {
      sndg[i].isPlaying = false;
    }
  }
}

int16_t processAndGetSample() {
  int32_t totalSamples = 0;
  uint32_t ph;
  bool decaying = false;

  // Process all sound generators
  for (uint8_t gen = 0; gen < NUMBER_OF_GENERATORS; gen++) {

    // Get phase for this sound generator
    ph = sndg[gen].phaseAccumulator;

    // Get wave table address
    uint32_t waveTableIndex = (ph >> POT);

    // Update phase with phasor
    sndg[gen].phaseAccumulator += sndg[gen].m;

    // Setup looping over sustain portion of wavetable
    // once we have reached end of wavetable data.
    if (sndg[gen].phaseAccumulator >= (((uint32_t)WAVETABLE_LENGTH) << POT)) {
      sndg[gen].phaseAccumulator -= (((uint32_t)SUSTAIN_LENGTH) << POT);
      decaying = true;
    }

    // Get the sample -127 .. 127
    int32_t s = (int32_t)waveTableData[waveTableIndex];

    // Get the envelope value 255 .. 0
    uint8_t e = envelopeData[sndg[gen].envelopeIndex];

    // Multiply sample times envelope value and add to sample sum
    totalSamples += s * (int32_t)e;

    // Advance envelope position if not at end of envelope table and in the process of decaying
    if ((e != 0) && decaying) {
      sndg[gen].envelopeIndex++;
    }
  }

  return (int16_t)(totalSamples / NUMBER_OF_GENERATORS);
}

void playSamples() {
  int16_t b = 0;
  for (uint16_t i = 0; i < SAMPLES_PER_BUFFER; i++) {
    // absolute_time_t before = get_absolute_time();
    b = processAndGetSample();
    i2s.write(b);
    i2s.write(b);
    // absolute_time_t after = get_absolute_time();
    // printf("time to generate sample: %d\n", (int32_t)absolute_time_diff_us(before, after));
  }
}

void setup() {
  // put your setup code here, to run once:
  Serial.begin(115200);

  delay(500);

  for(uint8_t i = 0; i < NUMBER_OF_GENERATORS; i++)
  {
    sndg[i].envelopeIndex = 0;
    sndg[i].isPlaying = false;
    sndg[i].isPlayingMidiNote = 0;
    sndg[i].m = 0;
    sndg[i].phaseAccumulator = 0;
  }

  Serial.println("Init I2S audio default...");
  i2s.setDATA(DEFAULT_pDOUT);
  i2s.setBCLK(DEFAULT_pBCLK);  // Note: LRCLK = BCLK + 1
  i2s.setBitsPerSample(16);
  i2s.begin(SAMPLE_RATE);

  // Initialize LittleFS:
  if (!LittleFS.begin()) {
    Serial.println("LittleFS init fail!");
    while (1)
      ;
  }

  // Initialize MIDIFile:
  SMF.begin(&LittleFS);
  SMF.setMidiHandler(midiCallback);

  Serial.println("Start playing song now!");

  // Load a MIDI file from there:
  int err = SMF.load("still_alive.mid");
  // int err = SMF.load("AiWaKatsu.mid");

  if (err != MD_MIDIFile::E_OK) {
    Serial.printf("SMF load error: %d", err);
    while (1)
      ;
  }

  Serial.printf("Filename: %s\n", SMF.getFilename());
  Serial.printf("Format: %d\n", SMF.getFormat());

  state = S_PLAYING;
}

void loop() {
  // put your main code here, to run repeatedly:
  switch (state) {
    case S_IDLE:
      delay(500);
      break;
    case S_PLAYING:
      playSamples();
      if (!SMF.isEOF()) {
        SMF.getNextEvent();
      } else {
        Serial.println("Playing done! Idle mode now...");
        state = S_IDLE;
      }
      break;
    default:
      break;
  }
}
