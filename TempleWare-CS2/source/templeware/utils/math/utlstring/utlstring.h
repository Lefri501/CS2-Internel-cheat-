#pragma once
template <size_t SIZE = 256>
struct String_t
{
    char szBuffer[SIZE];

    // Constructor for formatting a string
    String_t(const char* szFormat, const char* arg1, const char* arg2)
    {
        CustomFormat(szFormat, arg1, arg2);
    }

    // Custom format function (simplified for two arguments)
    void CustomFormat(const char* szFormat, const char* arg1, const char* arg2)
    {
        size_t idx = 0;
        const char* ptr = szFormat;

        // Loop through the format string
        while (*ptr != '\0' && idx < SIZE - 1)
        {
            if (*ptr == '%' && *(ptr + 1) == 's')  // Handle first "%s"
            {
                ptr += 2;  // Skip "%s"
                const char* strArg = arg1;
                while (*strArg != '\0' && idx < SIZE - 1)
                {
                    szBuffer[idx++] = *strArg++;
                }
            }
            else if (*ptr == '%' && *(ptr + 1) == 's')  // Handle second "%s"
            {
                ptr += 2;  // Skip "%s"
                const char* strArg = arg2;
                while (*strArg != '\0' && idx < SIZE - 1)
                {
                    szBuffer[idx++] = *strArg++;
                }
            }
            else
            {
                szBuffer[idx++] = *ptr++;  // Copy the current character
            }
        }

        szBuffer[idx] = '\0';  // Null terminate the string
    }

    // Getter for the formatted data
    const char* Data() const
    {
        return this->szBuffer;
    }

    // Clear the buffer
    void Clear()
    {
        szBuffer[0] = '\0';
    }
};