// Fixed callback function for libcurl
size_t write_callback(void *contents, size_t size, size_t nmemb, void *userp) {
    size_t total_size = size * nmemb;
    char **response = (char **)userp;
    
    // Get current length
    size_t current_len = strlen(*response);
    
    // Reallocate to append new data
    char *temp = realloc(*response, current_len + total_size + 1);
    if (!temp) return 0;
    
    *response = temp;
    // Append (not overwrite!)
    memcpy(*response + current_len, contents, total_size);
    (*response)[current_len + total_size] = '\0';
    
    return total_size;
}
