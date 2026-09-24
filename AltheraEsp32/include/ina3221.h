#pragma once
#include <Arduino.h>
#include <Wire.h>

// Driver mínimo INA3221 (sem dependência externa).
// Shunt: 40 µV/LSB, Bus: 8 mV/LSB, ambos com 13 bits alinhados à esquerda (bits 15..3).
class INA3221 {
public:
  explicit INA3221(uint8_t addr = 0x40) : _addr(addr) {}

  bool begin() {
    uint16_t id = 0;
    if (!read16(0xFE, id) || id != 0x5449) return false;   // Manufacturer ID "TI"
    // 3 canais ligados, média de 16 amostras, conversão 1.1 ms, modo contínuo
    return write16(0x00, 0x7527);
  }

  // ch = 1..3. Retorna tensão de barramento (V) e tensão no shunt (mV)
  bool readChannel(uint8_t ch, float &busV, float &shunt_mV) {
    uint16_t rs, rb;
    uint8_t reg = 1 + (ch - 1) * 2;
    if (!read16(reg, rs) || !read16(reg + 1, rb)) return false;
    shunt_mV = ((int16_t)rs >> 3) * 0.040f;
    busV     = ((int16_t)rb >> 3) * 0.008f;
    return true;
  }

  uint8_t address() const { return _addr; }

private:
  uint8_t _addr;

  bool read16(uint8_t reg, uint16_t &out) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    if (Wire.endTransmission(false) != 0) return false;
    if (Wire.requestFrom((int)_addr, 2) != 2) return false;
    out = (Wire.read() << 8) | Wire.read();
    return true;
  }

  bool write16(uint8_t reg, uint16_t val) {
    Wire.beginTransmission(_addr);
    Wire.write(reg);
    Wire.write(val >> 8);
    Wire.write(val & 0xFF);
    return Wire.endTransmission() == 0;
  }
};
