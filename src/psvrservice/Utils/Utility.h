#ifndef UTILITY_H
#define UTILITY_H

#include "stdlib.h" // size_t
#include <string>

//-- macros -----
#ifndef ARRAY_SIZE
#define ARRAY_SIZE(_A) (sizeof(_A) / sizeof((_A)[0]))
#endif

//-- utility methods -----
namespace Utility
{
    template <typename t_index>
    inline bool is_index_valid(const t_index index, const t_index count)
    {
        return index >= 0 && index < count;
    }

    unsigned char int32_to_int8_verify(int value);
    bool convert_wcs_to_mbs(const wchar_t *wc_string, char *out_mb_serial, const size_t mb_buffer_size);

    /// Formats a string into the given target buffer
    /// \param buffer The target buffer to write in to
    /// \param buffer_size The max number of bytes that can be written to the buffer
    /// \param format The formatting string that will be written to the buffer
    /// \return The number of characters successfully written
    int format_string(char *buffer, size_t buffer_size, const char *format, ...);

    /// Sets the name of the current thread
    void set_current_thread_name(const char* thread_name);

    /// Sleeps the current thread for the given number of milliseconds
    void sleep_ms(int milliseconds);	
};

#endif // UTILITY_H