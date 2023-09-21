#ifndef PERK_H_
#define PERK_H_


#ifdef _WIN32
    #include "perk_wasapi.h"
#endif


typedef void (*perk_write_callback_t)(float *buffer, unsigned size, void *data);

typedef struct
{
    perk_write_callback_t write_callback;
    void *user_data;
} perk_start_info_t;

typedef struct
{
    unsigned sample_frequency;
    unsigned channel_count;
} perk_format_info_t;

perk_format_info_t perk_init(void);

void perk_start(perk_start_info_t start_info);

void perk_exit(void);


#endif // PERK_H_
