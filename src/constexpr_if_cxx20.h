#ifndef DASH_CONSTEXPR_IF_CXX20_H
#define DASH_CONSTEXPR_IF_CXX20_H

#if __cplusplus >= 202002L
#define CONSTEXPR_IF_CPP20 constexpr
#else
#define CONSTEXPR_IF_CPP20
#endif

#endif //DASH_CONSTEXPR_IF_CXX20_H
