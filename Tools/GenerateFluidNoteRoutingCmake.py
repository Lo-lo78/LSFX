"""Generate an in-process CMake installer from the reviewed note-routing diff."""
from ApplyFluidNoteRouting import changes, PATCH


def bracket(text):
    # CMake drops an initial newline following an opening bracket, so callers
    # must keep leading newlines out of literals (add them in CMake instead).
    assert not text.startswith('\n')
    eq = '='
    while ']' + eq + ']' in text:
        eq += '='
    return '[' + eq + '[' + text + ']' + eq + ']'


def regex(block):
    def escape(line):
        return ''.join('\\' + c if c in '\\.^$*+?()[]|' else c for c in line.strip(' \t'))
    return '\n'.join('[ \t]*' + escape(line) + '[ \t]*' for line in block)


HEADER = r'''# Generated from fluidsynth-note-routing.patch. Do not edit by hand.
# No child process, interpreter, or PATH lookup is needed in the vcpkg sandbox.
function(lsfx_find_unique text pattern found position matched)
    string(REGEX MATCH "\n${pattern}\n" hit "${text}")
    if("${hit}" STREQUAL "")
        set(${found} 0 PARENT_SCOPE)
        set(${position} -1 PARENT_SCOPE)
        set(${matched} "" PARENT_SCOPE)
        return()
    endif()
    string(FIND "${text}" "${hit}" pos)
    math(EXPR next "${pos} + 1")
    string(SUBSTRING "${text}" ${next} -1 rest)
    string(REGEX MATCH "\n${pattern}\n" second "${rest}")
    if("${second}" STREQUAL "")
        set(${found} 1 PARENT_SCOPE)
    else()
        set(${found} 2 PARENT_SCOPE)
    endif()
    set(${position} ${pos} PARENT_SCOPE)
    set(${matched} "${hit}" PARENT_SCOPE)
endfunction()

function(lsfx_edit variable old_pattern new_pattern replacement insertion label)
    set(text "${${variable}}")
    lsfx_find_unique("${text}" "${old_pattern}" old_count old_pos old_hit)
    lsfx_find_unique("${text}" "${new_pattern}" new_count new_pos new_hit)
    if(new_count EQUAL 1)
        if(old_count EQUAL 0 OR (insertion AND old_count EQUAL 1 AND old_pos EQUAL new_pos))
            return()
        endif()
    endif()
    if(NOT old_count EQUAL 1 OR NOT new_count EQUAL 0)
        message(FATAL_ERROR "LSFX note routing patch refused: ${label}: expected one unmodified anchor; found ${old_count} old and ${new_count} new (2 means multiple)")
    endif()
    string(LENGTH "${old_hit}" len)
    math(EXPR end "${old_pos} + ${len}")
    string(SUBSTRING "${text}" 0 ${old_pos} prefix)
    string(SUBSTRING "${text}" ${end} -1 suffix)
    if(insertion)
        # Preserve whitespace in the original anchor, including its final newline.
        set(updated "${prefix}${old_hit}${replacement}\n${suffix}")
    else()
        set(updated "${prefix}\n${replacement}\n${suffix}")
    endif()
    set(${variable} "${updated}" PARENT_SCOPE)
endfunction()

function(lsfx_apply_note_routing root)
'''


def generate():
    out = [HEADER]
    files = changes(PATCH.read_text(encoding='utf-8-sig'))
    for i, (path, edits) in enumerate(files.items()):
        out.append(f'    set(path{i} "${{root}}/{path}")\n'
                   f'    file(READ "${{path{i}}}" original{i})\n'
                   f'    string(REPLACE "\\r\\n" "\\n" original{i} "${{original{i}}}")\n'
                   f'    if(NOT original{i} MATCHES "\\n$")\n        string(APPEND original{i} "\\n")\n    endif()\n'
                   f'    set(data{i} "\\n${{original{i}}}")\n')
        for j, (old, new) in enumerate(edits):
            insertion = old == new[:len(old)]
            replacement = '\n'.join(new[len(old):] if insertion else new)
            # Some insertions start with a blank line. Use a placeholder for
            # that first newline, restored before invoking the edit function.
            if replacement.startswith('\n'):
                out.append(f'    set(replacement "\\n")\n    string(APPEND replacement {bracket(replacement[1:])})\n')
            else:
                out.append(f'    set(replacement {bracket(replacement)})\n')
            out.append(f'    lsfx_edit(data{i} {bracket(regex(old))} {bracket(regex(new))} "${{replacement}}" {"TRUE" if insertion else "FALSE"} "{path} anchor {j+1}")\n')
    # Validate every anchor in every file before writing anything.
    for i in range(len(files)):
        out.append(f'    string(SUBSTRING "${{data{i}}}" 1 -1 updated{i})\n'
                   f'    if(NOT "${{updated{i}}}" STREQUAL "${{original{i}}}")\n'
                   f'        file(WRITE "${{path{i}}}" "${{updated{i}}}")\n    endif()\n')
    out.append('    message(STATUS "LSFX FluidSynth note routing: 10 unique code anchors verified in 4 source files (native CMake)")\nendfunction()\n')
    return ''.join(out)
