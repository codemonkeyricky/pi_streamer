#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include "logger.h"

void logger_log(
    const char *format, 
    ...
    )
{
    char    str[2048]; 


    // Consistency check. 
    assert(strlen(format) < 2048); 

    strcpy(str, format); 
    strcat(str, "\n"); 

    va_list args; 
    va_start(args, format); 

    vprintf(str, args); 

    va_end(args); 
}
