/*
  AudioGeneratorWAV
  Audio output generator that reads 8 and 16-bit WAV files
  
  Copyright (C) 2017  Earle F. Philhower, III

  This program is free software: you can redistribute it and/or modify
  it under the terms of the GNU General Public License as published by
  the Free Software Foundation, either version 3 of the License, or
  (at your option) any later version.

  This program is distributed in the hope that it will be useful,
  but WITHOUT ANY WARRANTY; without even the implied warranty of
  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
  GNU General Public License for more details.

  You should have received a copy of the GNU General Public License
  along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/


#include "AudioGeneratorWAV.h"

AudioGeneratorWAV::AudioGeneratorWAV()
{
  running = false;

  for(int i = 0; i < fileCount; i++){
    looping[i] = false;
    file[i] = NULL;
  }

  fileReadPtr = 0;
  fileWritePtr = 0;
  output = NULL;
  buffSize = 256; // Needs to be able to store the whole sound file??
  buff = NULL;
  buffPtr = 0;
  buffLen = 0;
}

AudioGeneratorWAV::~AudioGeneratorWAV()
{
  if (buff){
    free(buff);
    buff = NULL;
  }

  for (int i=0;i<fileCount;i++){
    if (file[i] != NULL){
      delete file[i];
    }
  }
}

void AudioGeneratorWAV::SetLoop(bool loopSet)
{
  looping[fileReadPtr] = loopSet;
}

bool AudioGeneratorWAV::isLooping()
{
  // log_i("fileReadPtr: %d, looping: %d", fileReadPtr, looping[fileReadPtr]);
  return looping[fileReadPtr];
}

bool AudioGeneratorWAV::setNextFile(AudioFileSource *source, bool looping, bool overwriteLooping)
{
  if (!source) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::setNextFile: failed: invalid source\n"));
    return false;
  }
  int i = fileReadPtr;
  bool removeFile = false;
  int newWritePointer = 0;
  // log_i("fileWritePtr: %d", fileWritePtr);
  while (i != fileWritePtr){
    if (removeFile){
      // log_i("Removing file %d", i);
      delete file[i];
      file[i] = NULL;
      this->looping[i] = false;
    } else {
      if (file[i]->getFilePointer() == source->getFilePointer()){
        // log_i("Same file %d", i);
        removeFile = true;
        newWritePointer = i + 1 == fileCount ? 0 : i + 1;
      }
    }
    i = i + 1 == fileCount ? 0 : i + 1;
  }
  if (removeFile){
    delete source;
    // log_i("oldWritePointer: %d newWritePointer: %d", fileWritePtr, newWritePointer);
    fileWritePtr = newWritePointer;
    return false;
  }
  int previousWritePointer = fileWritePtr - 1 == - 1 ? fileCount - 1 : fileWritePtr - 1;
  if (previousWritePointer != fileReadPtr && fileWritePtr != fileReadPtr){
    if (this->looping[previousWritePointer] && overwriteLooping){
      fileWritePtr = previousWritePointer;
    }
  } 

  if (file[fileWritePtr] != NULL){
    file[fileWritePtr]->close();
    delete file[fileWritePtr];
  }

  file[fileWritePtr] = source;
  this->looping[fileWritePtr] = looping;

  fileWritePtr = fileWritePtr + 1 == fileCount ? 0 : fileWritePtr + 1;

  return true;
}

bool AudioGeneratorWAV::NextFile()
{
  int nextFilePtr = fileReadPtr + 1 == fileCount ? 0 : fileReadPtr + 1;;
  if (file[nextFilePtr] == NULL){
    return false;
  } else {
    return true;
  }
}

bool AudioGeneratorWAV::stop()
{
  if (!running) return true;
  
  // log_i("fileReadPtr: %d, looping: %d", fileReadPtr, looping[fileReadPtr]);
  if (looping[fileReadPtr]){
    // Restart file 
    if (!file[fileReadPtr]->seek(file[fileReadPtr]->getSize() - fileBytes, SEEK_SET)){
      audioLogger->println("seek failed");
    }
    availBytes = fileBytes;

    return false;
  } else if (NextFile()){
    // Start next file 
    file[fileReadPtr]->close();
    delete file[fileReadPtr];
    file[fileReadPtr] = NULL;
    looping[fileReadPtr] = false;

    fileReadPtr = fileReadPtr + 1 == fileCount ? 0 : fileReadPtr + 1;
    int nextFilePointer = fileReadPtr + 1 == fileCount ? 0 : fileReadPtr + 1;
    
    if (looping[fileReadPtr] && file[nextFilePointer] != NULL){
      file[fileReadPtr]->close();
      delete file[fileReadPtr];
      file[fileReadPtr] = NULL;
      looping[fileReadPtr] = false;
      fileReadPtr = nextFilePointer;
    }

    // log_i("fileReadPtr: %d", fileReadPtr);


    if (!file[fileReadPtr]->isOpen()) {
      audioLogger->printf_P(PSTR("AudioGeneratorWAV::stop: file not open\n"));
      return false;
    } 

    if (!ReadWAVInfo()) {
      audioLogger->printf_P(PSTR("AudioGeneratorWAV::stop: failed during ReadWAVInfo\n"));
      return false;
    }
    return false;
  } else {
    // End file
    running = false;
    looping[fileReadPtr] = false;
    if (buff){
      free(buff);
      buff = NULL;
    }
    file[fileReadPtr]->close();
    delete file[fileReadPtr];
    file[fileReadPtr] = NULL;

    return true;
  }
}

bool AudioGeneratorWAV::isRunning()
{
  return running;
}


// Handle buffered reading, reload each time we run out of data
bool AudioGeneratorWAV::GetBufferedData(int bytes, void *dest)
{
  if (!running) return false; // Nothing to do here!
  uint8_t *p = reinterpret_cast<uint8_t*>(dest);
  while (bytes--) {
    // Potentially load next batch of data...
    if (buffPtr >= buffLen) {
      buffPtr = 0;
      uint32_t toRead = availBytes > buffSize ? buffSize : availBytes;
      buffLen = file[fileReadPtr]->read( buff, toRead );
      availBytes -= buffLen;
    }
    if (buffPtr >= buffLen){
      return false; // No data left!
    }
    if (buff){
      *(p++) = buff[buffPtr++];
    }
  }
  return true;
}

bool AudioGeneratorWAV::loop()
{
  if (!running) goto done; // Nothing to do here!

  // Try and stuff the buffer one sample at a time
  do
  {
    if (bitsPerSample == 8) {
      uint8_t l, r;
      if (!GetBufferedData(1, &l)) stop();
      if (channels == 2) {
        if (!GetBufferedData(1, &r)) stop();
      } else {
        r = 0;
      }
      lastSample[AudioOutput::LEFTCHANNEL] = l;
      lastSample[AudioOutput::RIGHTCHANNEL] = r;
    } else if (bitsPerSample == 16) {
      if (!GetBufferedData(2, &lastSample[AudioOutput::LEFTCHANNEL])) stop();
      if (channels == 2) {
        if (!GetBufferedData(2, &lastSample[AudioOutput::RIGHTCHANNEL])) stop();
      } else {
        lastSample[AudioOutput::RIGHTCHANNEL] = 0;
      }
    }
  } while (running && output->ConsumeSample(lastSample));

done:
  if (file[fileReadPtr] != NULL){
    file[fileReadPtr]->loop();
  }
  output->loop();

  return running;
}


bool AudioGeneratorWAV::ReadWAVInfo()
{
  uint32_t u32;
  uint16_t u16;
  int toSkip;

  // WAV specification document:
  // https://www.aelius.com/njh/wavemetatools/doc/riffmci.pdf

  // Header == "RIFF"
  if (!ReadU32(&u32)) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
    return false;
  };
  if (u32 != 0x46464952) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: cannot read WAV, invalid RIFF header, got: %08X \n"), (uint32_t) u32);
    return false;
  }

  // Skip ChunkSize
  if (!ReadU32(&u32)) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
    return false;
  };

  // Format == "WAVE"
  if (!ReadU32(&u32)) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
    return false;
  };
  if (u32 != 0x45564157) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: cannot read WAV, invalid WAVE header, got: %08X \n"), (uint32_t) u32);
    return false;
  }

  // there might be JUNK or PAD - ignore it by continuing reading until we get to "fmt "
  while (1) {
    if (!ReadU32(&u32)) {
      audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
      return false;
    };
    if (u32 == 0x20746d66) break; // 'fmt '
  };

  // subchunk size
  if (!ReadU32(&u32)) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
    return false;
  };
  if (u32 == 16) { toSkip = 0; }
  else if (u32 == 18) { toSkip = 18 - 16; }
  else if (u32 == 40) { toSkip = 40 - 16; }
  else {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: cannot read WAV, appears not to be standard PCM \n"));
    return false;
  } // we only do standard PCM

  // AudioFormat
  if (!ReadU16(&u16)) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
    return false;
  };
  if (u16 != 1) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: cannot read WAV, AudioFormat appears not to be standard PCM \n"));
    return false;
  } // we only do standard PCM

  // NumChannels
  if (!ReadU16(&channels)) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
    return false;
  };
  if ((channels<1) || (channels>2)) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: cannot read WAV, only mono and stereo are supported \n"));
    return false;
  } // Mono or stereo support only

  // SampleRate
  if (!ReadU32(&sampleRate)) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
    return false;
  };
  if (sampleRate < 1) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: cannot read WAV, unknown sample rate \n"));
    return false;
  }  // Weird rate, punt.  Will need to check w/DAC to see if supported

  // Ignore byterate and blockalign
  if (!ReadU32(&u32)) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
    return false;
  };
  if (!ReadU16(&u16)) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
    return false;
  };

  // Bits per sample
  if (!ReadU16(&bitsPerSample)) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
    return false;
  };
  if ((bitsPerSample!=8) && (bitsPerSample != 16)) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: cannot read WAV, only 8 or 16 bits is supported \n"));
    return false;
  }  // Only 8 or 16 bits

  // Skip any extra header
  while (toSkip) {
    uint8_t ign;
    if (!ReadU8(&ign)) {
      audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
      return false;
    };
    toSkip--;
  }

  // look for data subchunk
  do {
    // id == "data"
    if (!ReadU32(&u32)) {
      audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
      return false;
    };
    if (u32 == 0x61746164) break; // "data"
    // Skip size, read until end of chunk
    if (!ReadU32(&u32)) {
      audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
      return false;
    };
    if(!file[fileReadPtr]->seek(u32, SEEK_CUR)) {
      audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data, seek failed\n"));
      return false;
    }
  } while (1);
  if (!file[fileReadPtr]->isOpen()) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: cannot read WAV, file is not open\n"));
    return false;
  };

  // Skip size, read until end of file...
  if (!ReadU32(&u32)) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: failed to read WAV data\n"));
    return false;
  };
  availBytes = u32;
  fileBytes = u32;

  // Now set up the buffer or fail
  if (buff != NULL){
    free(buff);
    buff = NULL;
  }
  buff = reinterpret_cast<uint8_t *>(malloc(buffSize));
  if (!buff) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::ReadWAVInfo: cannot read WAV, failed to set up buffer \n"));
    return false;
  };
  buffPtr = 0;
  buffLen = 0;

  return true;
}

bool AudioGeneratorWAV::begin(AudioFileSource *source, AudioOutput *output)
{
  fileWritePtr = 0;
  fileReadPtr = 0;
  if (!source) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::begin: failed: invalid source\n"));
    return false;
  }
  file[fileWritePtr] = source;
  if (!output) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::begin: invalid output\n"));
    return false;
  }
  this->output = output;
  if (!file[fileWritePtr]->isOpen()) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::begin: file not open\n"));
    return false;
  } // Error

  if (!ReadWAVInfo()) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::begin: failed during ReadWAVInfo\n"));
    return false;
  }

  if (!output->SetRate( sampleRate )) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::begin: failed to SetRate in output\n"));
    return false;
  }
  if (!output->SetBitsPerSample( bitsPerSample )) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::begin: failed to SetBitsPerSample in output\n"));
    return false;
  }
  if (!output->SetChannels( channels )) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::begin: failed to SetChannels in output\n"));
    return false;
  }
  if (!output->begin()) {
    audioLogger->printf_P(PSTR("AudioGeneratorWAV::begin: output's begin did not return true\n"));
    return false;
  }

  fileWritePtr = fileWritePtr + 1 == fileCount ? 0 : fileWritePtr + 1;
  // log_i("fileWritePtr: %d", fileWritePtr);
  running = true;

  return true;
}
