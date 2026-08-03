/*
 * OpenKeyboard.org OpenDongle
 * Copyright 2026 Eric Molitor (EMulator)
 * CH592 RF-PHY HAL seam — private declarations.
 *
 * CH59x implements hal_rf.h over the LIBCH59xBLE radio (RF_Config /
 * RF_SetChannel / RF_Rx / RF_Tx / RF_Shut). The vendor status callback
 * (RF_2G4StatusCallBack) lives in hal_rf_ch592.c reduced to pure PHY-event
 * extraction; rf_task registers its protocol sink via hal_rf_set_event_cb().
 */
#ifndef HAL_RF_CH592_H
#define HAL_RF_CH592_H

#include "hal_rf.h"

#endif /* HAL_RF_CH592_H */
