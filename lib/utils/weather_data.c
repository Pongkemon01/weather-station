// Fixed-point functions should be implemented here
#define _FIXEDPT_IMPLEMENTATION

#include "weather_data.h"

#define LARGEST_FLOAT (fixedpt_tofloat(FIXEDPT_LARGEST))
#define SMALLEST_FLOAT (fixedpt_tofloat(FIXEDPT_SMALLEST))

static fixedpt float_to_fixedpt(float f)
{
    // Range limiting
    if (f >= LARGEST_FLOAT)
        return(FIXEDPT_LARGEST);
    if (f <= SMALLEST_FLOAT)
        return(FIXEDPT_SMALLEST);
    
    // Valid range, return the conversion
    return(fixedpt_rconst(f));
}

static inline float fixedpt_to_float(fixedpt f)
{
    return(fixedpt_tofloat(f));
}

void PackData(Weather_Data_t *data, Weather_Data_Packed_t *packed)
{
    packed->time_stamp = get_epoch_from_datetime(&(data->sampletime));
    packed->temperature = float_to_fixedpt(data->temperature);
    packed->humidity = float_to_fixedpt(data->humidity);
    packed->pressure = float_to_fixedpt(data->pressure);
    packed->light_par = data->light_par;
    packed->rainfall = float_to_fixedpt(data->rainfall);
    packed->dew_point = float_to_fixedpt(data->dew_point);
    packed->bus_value = float_to_fixedpt(data->bus_value);
}

void UnpackData(Weather_Data_Packed_t *packed, Weather_Data_t *data)
{
    get_datetime_from_epoch(packed->time_stamp, &(data->sampletime));
    data->temperature = fixedpt_to_float(packed->temperature);
    data->humidity = fixedpt_to_float(packed->humidity);
    data->pressure = fixedpt_to_float(packed->pressure);
    data->light_par = packed->light_par;
    data->rainfall = fixedpt_to_float(packed->rainfall);
    data->dew_point = fixedpt_to_float(packed->dew_point);
    data->bus_value = fixedpt_to_float(packed->bus_value);
}
