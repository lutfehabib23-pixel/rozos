/* rozOS — cJSON-এর ছোট উদাহরণ (rozpkg install cjson-এর পরে)
 *   tcc /usr/share/doc/cjson/example.c -lcjson -o /tmp/j && /tmp/j */

#include <stdio.h>
#include <stdlib.h>
#include <cjson/cJSON.h>

int main(void)
{
    const char *text = "{\"os\":\"rozOS\",\"phase\":10,\"pi\":3.14159,\"features\":[\"tcc\",\"tls\",\"rozpkg\"]}";
    cJSON *root = cJSON_Parse(text), *it, *obj;
    char *out;

    if (!root) {
        printf("parse error near: %s\n", cJSON_GetErrorPtr());
        return 1;
    }
    printf("os      = %s\n", cJSON_GetObjectItem(root, "os")->valuestring);
    printf("phase   = %d\n", cJSON_GetObjectItem(root, "phase")->valueint);
    printf("pi      = %g\n", cJSON_GetObjectItem(root, "pi")->valuedouble);
    printf("features:");
    cJSON_ArrayForEach(it, cJSON_GetObjectItem(root, "features")) printf(" %s", it->valuestring);
    printf("\n");
    cJSON_Delete(root);

    obj = cJSON_CreateObject();
    cJSON_AddStringToObject(obj, "hello", "world");
    cJSON_AddNumberToObject(obj, "answer", 42);
    cJSON_AddBoolToObject(obj, "ok", 1);
    out = cJSON_Print(obj);
    printf("%s\n", out);
    free(out);
    cJSON_Delete(obj);
    return 0;
}
