#include "BMSModule.hpp"
#include "BMSDriver.hpp"
#include "Logger.hpp"


BMSModule::BMSModule()
{
  resetRecordedValues();
  moduleAddress = 0;
}

/*
   This function would be called via the serial port or a switch to reset watermarks
*/
void BMSModule::resetRecordedValues()
{
  for (int i = 0; i < 6; i++)
  {
    cellVolt[i] = 0.0f;
    lowestCellVolt[i] = 5.0f;
    highestCellVolt[i] = 0.0f;
  }
  moduleVolt = 0.0f;
  retmoduleVolt = 0.0f;
  temperatures[0] = 0.0f;
  temperatures[1] = 0.0f;
  lowestTemperature = 200.0f;
  highestTemperature = -100.0f;
  lowestModuleVolt = 200.0f;
  highestModuleVolt = 0.0f;
}

/*
   This function synchronizes the physical battery mdule with the object instance
*/
bool BMSModule::updateInstanceWithModuleValues()
{
  uint8_t buff[50];
  int16_t err;
  float tempCalc;
  float tempTemp;

  /*
     Status registers
  */
  //BMSDR(moduleAddress, REG_DEV_STATUS, 1, buff);
  //BMSDR(moduleAddress, REG_BAL_TIME, 1, buff);
  if ((err = BMSDR(moduleAddress, REG_ALERT_STATUS, 4, buff)) > 0) {
    alerts = buff[0];
    faults = buff[1];
    COVFaults = buff[2];
    CUVFaults = buff[3];
    LOG_DEBUG("Module %i   alerts=%X   faults=%X   COV=%X   CUV=%X", moduleAddress, alerts, faults, COVFaults, CUVFaults);
  } else {
    BMSD_LOG_ERR(moduleAddress, err, "Reading Status Registers");
    return false;
  }

  /*
     Voltage and Temperature registers
  */
  //ADC Auto mode, read every ADC input we can (Both Temps, Pack, 6 cells)
  if ((err = BMSDW(moduleAddress, REG_ADC_CTRL, 0b00111101)) < 0) {
    BMSD_LOG_ERR(moduleAddress, err, "ADC Auto mode");
    return false;
  }

  //enable temperature measurement VSS pins
  if ((err = BMSDW(moduleAddress, REG_IO_CTRL, 0b00000011)) < 0) {
    BMSD_LOG_ERR(moduleAddress, err, "enable temperature measurement VSS pins");
    return false;
  }

  //start all ADC conversions
  if ((err = BMSDW(moduleAddress, REG_ADC_CONV, 1)) < 0) {
    BMSD_LOG_ERR(moduleAddress, err, "start all ADC conversions");
    return false;
  }

  //start reading registers at the module voltage registers
  //read 18 bytes (Each value takes 2 - ModuleV, CellV1-6, Temp1, Temp2)
  if ((err = BMSDR(moduleAddress, REG_GPAI, 0x12, buff)) > 0) {
    //payload is 2 bytes gpai, 2 bytes for each of 6 cell voltages, 2 bytes for each of two temperatures (18 bytes of data)
    retmoduleVolt = (buff[0] * 256 + buff[1]) * 0.0020346293922562f;///0.002034609f;
    if (retmoduleVolt > highestModuleVolt) highestModuleVolt = retmoduleVolt;
    if (retmoduleVolt < lowestModuleVolt) lowestModuleVolt = retmoduleVolt;
    for (int i = 0; i < 6; i++)
    {
      cellVolt[i] = (buff[2 + (i * 2)] * 256 + buff[3 + (i * 2)]) * 0.000381493f;
      if (lowestCellVolt[i] > cellVolt[i]) lowestCellVolt[i] = cellVolt[i];
      if (highestCellVolt[i] < cellVolt[i]) highestCellVolt[i] = cellVolt[i];
    }

    //use added up cells and not reported module voltage
    moduleVolt = 0;
    for (int i = 0; i < 6; i++)
    {
      moduleVolt = moduleVolt + cellVolt[i];
    }
  } else {
    BMSD_LOG_ERR(moduleAddress, err, "Reading voltage registers");
    return false;
  }

  //Now using steinhart/hart equation for temperatures. We'll see if it is better than old code.
  tempTemp = (1.78f / ((buff[14] * 256 + buff[15] + 2) / 33046.0f) - 3.57f);
  tempTemp *= 1000.0f;
  tempCalc =  1.0f / (0.0007610373573f + (0.0002728524832 * logf(tempTemp)) + (powf(logf(tempTemp), 3) * 0.0000001022822735f));

  temperatures[0] = tempCalc - 273.15f;

  tempTemp = 1.78f / ((buff[16] * 256 + buff[17] + 9) / 33068.0f) - 3.57f;
  tempTemp *= 1000.0f;
  tempCalc = 1.0f / (0.0007610373573f + (0.0002728524832 * logf(tempTemp)) + (powf(logf(tempTemp), 3) * 0.0000001022822735f));
  temperatures[1] = tempCalc - 273.15f;

  if (getLowTemp() < lowestTemperature) lowestTemperature = getLowTemp();
  if (getHighTemp() > highestTemperature) highestTemperature = getHighTemp();

  LOG_DEBUG("Got voltage and temperature readings");
  //TODO: turning the temperature wires off here seems to cause weird temperature glitches
  return true;
}

uint8_t BMSModule::getFaults()
{
  return faults;
}

uint8_t BMSModule::getAlerts()
{
  return alerts;
}

uint8_t BMSModule::getCOVCells()
{
  return COVFaults;
}

uint8_t BMSModule::getCUVCells()
{
  return CUVFaults;
}

float BMSModule::getCellVoltage(int cell)
{
  if (cell < 0 || cell > 5) return 0.0f;
  return cellVolt[cell];
}

float BMSModule::getLowCellV()
{
  float lowVal = 10.0f;
  for (int i = 0; i < 6; i++) if (cellVolt[i] < lowVal) lowVal = cellVolt[i];
  return lowVal;
}

float BMSModule::getHighCellV()
{
  float hiVal = 0.0f;
  for (int i = 0; i < 6; i++) if (cellVolt[i] > hiVal) hiVal = cellVolt[i];
  return hiVal;
}

float BMSModule::getAverageV()
{
  int x = 0;
  float avgVal = 0.0f;
  for (int i = 0; i < 6; i++)
  {
    if (cellVolt[i] < 60.0)
    {
      x++;
      avgVal += cellVolt[i];
    }
  }

  avgVal /= x;
  return avgVal;
}

float BMSModule::getHighestModuleVolt()
{
  return highestModuleVolt;
}

float BMSModule::getLowestModuleVolt()
{
  return lowestModuleVolt;
}

float BMSModule::getHighestCellVolt(int cell)
{
  if (cell < 0 || cell > 5) return 0.0f;
  return highestCellVolt[cell];
}

float BMSModule::getLowestCellVolt(int cell)
{
  if (cell < 0 || cell > 5) return 0.0f;
  return lowestCellVolt[cell];
}

float BMSModule::getHighestTemp()
{
  return highestTemperature;
}

float BMSModule::getLowestTemp()
{
  return lowestTemperature;
}

float BMSModule::getLowTemp()
{
  return (temperatures[0] < temperatures[1]) ? temperatures[0] : temperatures[1];
}

float BMSModule::getHighTemp()
{
  return (temperatures[0] < temperatures[1]) ? temperatures[1] : temperatures[0];
}

float BMSModule::getAvgTemp()
{
  return (temperatures[0] + temperatures[1]) / 2.0f;
}

float BMSModule::getModuleVoltage()
{
  return moduleVolt;
}

float BMSModule::getTemperature(int temp)
{
  if (temp < 0 || temp > 1) return 0.0f;
  return temperatures[temp];
}

void BMSModule::setAddress(int newAddr)
{
  if (newAddr < 0 || newAddr > MAX_MODULE_ADDR) return;
  moduleAddress = newAddr;
}

int BMSModule::getAddress()
{
  return moduleAddress;
}