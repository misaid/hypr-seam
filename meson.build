project('hypr-seam', 'cpp',
  version: '0.1',
  default_options: ['cpp_std=c++23', 'warning_level=2'])

hyprland = dependency('hyprland')
pixman = dependency('pixman-1')

sources = [
  'src/main.cpp',
  'src/SeamConfig.cpp',
  'src/SeamRuleStore.cpp',
  'src/Adjacency.cpp',
  'src/SeamState.cpp',
  'src/SeamHook.cpp',
  'src/SeamDecorHook.cpp',
]

shared_module('hypr-seam', sources,
  dependencies: [hyprland, pixman],
  install: false)
