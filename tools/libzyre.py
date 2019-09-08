import generic

def options(opt):
    generic._options(opt, 'libzyre')

def configure(cfg):
    generic._configure(cfg, 'libzyre', incs=('zyre.h',), libs=('zyre',),
                       pcname='libzyre',
                       uses = 'LIBCZMQ', mandatory=True)
