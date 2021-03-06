#include <memory>

#include "dataclasses/value.h"
#include "dataclasses/datetime.h"
#include "dataclasses/sensordata.h"
#include "hardwareclasses/tdssensor.h"
#include "hardwareclasses/phsensor.h"
#include "hardwareclasses/buoyble.h"
#include "hardwareclasses/loramodule.h"
#include "hardwareclasses/rasppi.h"
#include "hardwareclasses/buoy.h"
#include "raspicom/raspcommands.h"
#include "manager.h"

#define M_INTERVAL_SECONDS 10

volatile int measurement_timer_counter;
volatile int measurement_timer_total;
hw_timer_t* measurement_timer = NULL;
portMUX_TYPE measurement_timer_mux = portMUX_INITIALIZER_UNLOCKED;

/**
 * Callback function for measurement timer interrupt
 */
void IRAM_ATTR measurementCallback() {
    portENTER_CRITICAL_ISR(&measurement_timer_mux);
    measurement_timer_counter++;
    measurement_timer_total++;
    portEXIT_CRITICAL_ISR(&measurement_timer_mux);
}


Manager::Manager() {
    _startTransmission = false;
}


void Manager::setupTimers() {
    Serial.println("Registering interrupts");
    measurement_timer = timerBegin(0, 80, true);
    timerAttachInterrupt(measurement_timer, &measurementCallback, true);
    timerAlarmWrite(measurement_timer, M_INTERVAL_SECONDS * 1000000, true);
    timerAlarmEnable(measurement_timer);
}


void Manager::takeMeasurements() {

    // poll time and location from GPS module
    Location_t location = _gpssensor->get_location();
    DateTime datetime = _gpssensor->get_datetime();

    // sample all sensors and create SensorData instance
    auto all_values = _buoy->sampleAllSensors();
    auto buoy_id = _buoy->get_buoy_id();
    auto last_id = _sdcard->get_buoy_states().at(buoy_id).second;
    SensorData sensordata(buoy_id, last_id, location, datetime, all_values);

    // print to json string to Serial and save to sdcard
    Serial.println("Printing obtained SensorData: ");
    Serial.println(sensordata.toJsonString().c_str());
    _sdcard->writeSensorData(sensordata);
}


void Manager::dumpMeasurements() {
    Serial.println("Initiating data dump to Rasppi with MetaData: ");
    Serial.println(_sdcard->_meta_data->toJsonString().c_str());
    
    _rasppi->turnOn();

    auto buoy_states = _sdcard->get_buoy_states();

    for (auto const& entry : buoy_states) {
        auto buoy_id = entry.first;
        auto first_last = entry.second;
        SensorData sensordata;

        for (uint32_t m_idx = first_last.first; m_idx < first_last.second; m_idx++) {
            sensordata = _sdcard->readSensorData(buoy_id, m_idx);
            if (sensordata) {
                Serial.printf("For buoy %u loaded measurement %u successfully \n", (uint) buoy_id, (uint) m_idx);
                _rasppi->waitForReady();
                _rasppi->writeData(sensordata.toPiJsonString());
            } else {
                Serial.printf("For buoy %u couldn't load measurement %u\n", (uint) buoy_id, (uint) m_idx);
            }
        }
    }

    while (_buoyble->getValue_bool());
    _rasppi->turnOff();
}


void Manager::createObjects() {

    _buoy = std::make_shared<Buoy>(1);
    _rasppi = std::make_shared<RaspPi>();
    _sdcard = std::make_shared<SDCard>(_buoy->get_buoy_id());
    _gpssensor = std::make_shared<GPSSensor>();
    _buoyble = std::make_shared<BuoyBLE>();
    _lora = std::make_shared<LoraModule>();

    _buoy->attachSensor(std::make_shared<TDSSensor>(0));
    _buoy->attachSensor(std::make_shared<PHSensor>(1));
    
    _sdcard->init();
    _gpssensor->init();
    _buoyble->init();

    _sdcard->loadMetaData();

}


void Manager::execute() {

    // check if measurement timer has set its flag
    if (measurement_timer_counter > 0) {
        portENTER_CRITICAL_ISR(&measurement_timer_mux);
        measurement_timer_counter--;
        portEXIT_CRITICAL_ISR(&measurement_timer_mux);
        Serial.println("Measurement Interrupt called!");
        takeMeasurements();
    }

    // wait for ping from BLE module
    if (_buoyble->getValue_bool()) {
        dumpMeasurements();
    }
}