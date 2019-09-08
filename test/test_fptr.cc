#include <czmq.h>

extern "C" {
    typedef int(my_func_t) (int, void*);

    int call_it(my_func_t f, int i, void* vo) {
        return f(i, vo);
    }
}

struct my_type_t {
    int meth(int i) { return i+1; }
};

template<typename T, int (T::*meth)(int)>
int tmeth(int i, void* vo) {
    T* o = (T*)vo;
    return (o->*meth)(i);
}

int main()
{
    my_type_t mt;

    int o = call_it(tmeth<my_type_t, &my_type_t::meth>, 1, (void*)&mt);
    assert(o == 2);

    return 0;
}
