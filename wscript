def options(opt):
    opt.load('g++')

def configure(cfg):
    cfg.load('g++')

def build(bld):
    bld.objects(
        target = 'vercl',
        source = 'vercl/RealtimeComm.cpp',
        cxxflags = '-g -std=gnu++11',
        export_includes = '.'
    )

    bld.recurse('test')
