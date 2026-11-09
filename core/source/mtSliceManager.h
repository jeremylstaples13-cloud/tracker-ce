#ifndef SOURCE_MTSLICEMANAGER_H_
#define SOURCE_MTSLICEMANAGER_H_
#include "SliceFades.h"
#include "mtStructs.h"

class mtSliceManager
{
  public:
    void addSlice(strInstrument* instr);
    void removeSlice(strInstrument* instr);
    void adjustSlice(strInstrument* instr, int32_t val);
    void autoSlice(strInstrument* instr);
    void equalSlice(strInstrument* instr, uint8_t n);

    uint8_t getAutoSliceProgress();
    uint8_t getAutoSliceState();

    // Call once when auto-slice detection has completed.
    void onAutoSliceFinished();

  private:
    void addSliceInternal(strInstrument* instr);
    void removeSliceInternal(strInstrument* instr);
    void adjustSliceInternal(strInstrument* instr, int32_t val);

    uint8_t pendingFadeInstr = UINT8_MAX;
    SliceFades::Fade pendingFade = { 0, 0 };
};

extern mtSliceManager sliceManager;

#endif /* SOURCE_MTSLICEMANAGER_H_ */
