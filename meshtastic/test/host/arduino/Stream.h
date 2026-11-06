#pragma once
#include "Arduino.h"

class Stream : public Print
{
  public:
    virtual int available() = 0;
    virtual int read() = 0;
    virtual int read(uint8_t *, size_t) = 0;
    virtual int peek() = 0;
    virtual void flush() = 0;
};
