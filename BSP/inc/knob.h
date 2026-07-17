//Created by kk on 2026/7/8 10:40

#ifndef __KNOB_H__
#define __KNOB_H__

typedef enum{
    KNOB_DIR_NONE=0,
	KNOB_DIR_LEFT=1,
	KNOB_DIR_RIGHT=2,
}KnobDirection;
void Knob_Init(void);
KnobDirection Knob_Direction(void);

#endif 