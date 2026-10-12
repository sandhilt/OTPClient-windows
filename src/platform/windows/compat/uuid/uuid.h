#pragma once
typedef unsigned char uuid_t[16];
void uuid_generate_random (uuid_t uuid);
void uuid_unparse_lower (const uuid_t uuid, char *text);
