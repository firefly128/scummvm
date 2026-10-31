#pragma once
inline void warning(const char*,...){ }
inline void error(const char*s){fprintf(stderr,"%s\n",s);abort();}
