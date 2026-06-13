import os
import sys
import re
import json
import argparse

# Global compiler state
oBB = []
current_instruction_index = 0
current_line = ""
default_delay = 0
delay_override = False
total_eval_registers = 0
free_eval_registers = 0
current_block = 0x0000
next_available_block_address = 0x0000
block_stack = []
awaiting_future_addr_assignment = []
next_available_var_address = 0x0001
var_map = {}
var_values = ['VALUES']
requires_lang_pack = False
label_map = {}
goto_awaiting_future_label = {}
defining_function = False
button_block_stack = []
button_block_counter = 0
defining_button = 0
defining_button_at = 0
return_defined = False
preprocessor_labels = []
preprocessor_replacements = []

INSIDE_REM_BLOCK = False
INSIDE_STRING_BLOCK = False
INSIDE_STRINGLN_BLOCK = False
string_block_indentation_level = 1
preserve_leading_space = False
block_started_at = 0

# Constants and Maps
EVAL_VAR = '$__r'

double_operator = ['&&', '||', '<<', '>>', '==', '!=', '()', '<=', '>=' ]

operator_map = {
    '+':    0x02E8, # add
    '-':    0x03E8, # sub
    '*':    0x04E8, # multiply
    '/':    0x05E8, # divide
    '==':   0x06E8, # equality
    '!=':   0x07E8, # !equality
    '<':    0x08E8, # less than
    '>':    0x09E8, # greater than
    '<=':   0xA8E8, # less than
    '>=':   0xA9E8, # greater than
    '&&':   0xAAE8, # logical and
    'AND':  0xAAE8, # logical and
    '||':   0xBBE8, # logical or
    'OR':   0xBBE8, # logical or
    '&':    0x0AE8, # bitwise and
    '|':    0x0BE8, # bitwise or
    '>>':   0x0CE8, # shift >>
    '<<':   0x0DE8, # shift <<
    '%':    0x0EE8, # mod
    '^':    0xE80F  # power
}
operator_keys = list(operator_map.keys())

reserved_variables = {
    '$_BUTTON_ENABLED':                              0x4280,
    '$_BUTTON_USER_DEFINED':                         0x4281,
    '$_BUTTON_PUSH_RECEIVED':                        0x4284,
    '$_LED_SHOW_STORAGE_ACTIVITY':                   0x4285,
    '$_SYSTEM_LEDS_ENABLED':                         0x4286,
    '$_STORAGE_LEDS_ENABLED':                        0x4287,
    '$_INJECTING_LEDS_ENABLED':                      0x4288,
    '$_EXFIL_LEDS_ENABLED':                          0x4289,
    '$_CAPSLOCK_ON':                                 0x4290,
    '$_NUMLOCK_ON':                                  0x4291,
    '$_SCROLLLOCK_ON':                               0x4292,
    '$_SAVED_CAPSLOCK_ON':                           0x4293,
    '$_SAVED_NUMLOCK_ON':                            0x4294,
    '$_SAVED_SCROLLLOCK_ON':                         0x4295,
    '$_RECEIVED_HOST_LOCK_LED_REPLY':                0x4296,
    '$_EXFIL_MODE_ENABLED':                          0x4297,
    '$_STORAGE_ACTIVITY_TIMEOUT':                    0x4298,
    '$_BUTTON_TIMEOUT':                              0x4299,
    '$_PAYLOAD_PARSE_SPEED':                         0x429A,
    '$_CURRENT_VID':                                 0x429B,
    '$_CURRENT_PID':                                 0x429C,
    '$_OS':                                          0x429D,
    '$_HOST_CONFIGURATION_REQUEST_COUNT':            0x429F,
    '$_CURRENT_ATTACKMODE':                          0x42A0,
    '$_JITTER_ENABLED':                              0x42A2,
    '$_JITTER_MAX':                                  0x42A3,
    '$_STORAGE_ACTIVE':                              0x42A4,
    '$_RANDOM_INT':                                  0x42A8,
    '$_RANDOM_MIN':                                  0x42F0,
    '$_RANDOM_MAX':                                  0x42F1,
    '$_RANDOM_SEED':                                 0x42F2,
    '$_RANDOM_UINT16':                               0x42F3,
    '$_RANDOM_ASCII_LOWER_LETTER':                   0x42F4,
    '$_RANDOM_ASCII_UPPER_LETTER':                   0x42F5,
    '$_RANDOM_ASCII_LETTER':                         0x42F6,
    '$_RANDOM_ASCII_NUMBER':                         0x42F7,
    '$_RANDOM_ASCII_SPECIAL':                        0x42F8,
    '$_RANDOM_ASCII_CHAR':                           0x42F9,
    '$_RANDOM_LOWER_LETTER_KEYCODE':                      0x42FA,
    '$_RANDOM_UPPER_LETTER_KEYCODE':                      0x42FB,
    '$_RANDOM_LETTER_KEYCODE':                            0x42FF,
    '$_RANDOM_NUMBER_KEYCODE':                            0x42FC,
    '$_RANDOM_SPECIAL_KEYCODE':                           0x42FD,
    '$_RANDOM_CHAR_KEYCODE':                              0x42FE,
}
reserved_variables_list = list(reserved_variables.keys())
requires_lang_pack_list = [
    '$_RANDOM_LOWER_LETTER_KEYCODE',
    '$_RANDOM_UPPER_LETTER_KEYCODE',
    '$_RANDOM_LETTER_KEYCODE',
    '$_RANDOM_NUMBER_KEYCODE',
    '$_RANDOM_SPECIAL_KEYCODE',
    '$_RANDOM_CHAR_KEYCODE'
]

reserved_constants = {
    "FALSE":    0x4267,
    "TRUE":     0x4268,
    "WINDOWS":  0x4269,
    "MACOS":    0x4270,
    "LINUX":    0x4271,
    "ANDROID":  0x4272,
    "IOS":      0x4273,
    "CHROMEOS": 0x4274,
}
reserved_constant_list = list(reserved_constants.keys())

builtins_map = {
    'RESET':                                    0xED04,
    'KEY_HOLD':                                 0xF8FF,
    'KEY_RELEASE':                              0xE8EE,
    'DISABLE_BUTTON':                           0xEEEB,
    'ENABLE_BUTTON':                            0xEEEC,
    'RESTART_PAYLOAD':                          0xF1EA,
    'STOP_PAYLOAD':                             0xF1EB,
    'LED_OFF':                                  0xEDEA,
    'LED_G':                                    0xEDEB,
    'LED_GREEN':                                0xEDEB,
    'LED_R':                                    0xEDEC,
    'LED_RED':                                  0xEDEC,
    'ENABLE_SYSTEM_LEDS':                       0xED01,
    'DISABLE_SYSTEM_LEDS':                      0xED02,
    'SAVE_HOST_KEYBOARD_LOCK_STATE':            0xEBEA,
    'RESTORE_HOST_KEYBOARD_LOCK_STATE':         0xEBEB,
    'WAIT_FOR_BUTTON_PRESS':                    0xEAEA,
    'SAVE_ATTACKMODE':                          0xE9EA,
    'RESTORE_ATTACKMODE':                       0xE9EB,
    'WAIT_FOR_CAPS_ON':                         0xEA01,
    'WAIT_FOR_CAPS_OFF':                        0xEA02,
    'WAIT_FOR_CAPS_CHANGE':                     0xEA03,
    'WAIT_FOR_NUM_ON':                          0xEA04,
    'WAIT_FOR_NUM_OFF':                         0xEA05,
    'WAIT_FOR_NUM_CHANGE':                      0xEA06,
    'WAIT_FOR_SCROLL_ON':                       0xEA07,
    'WAIT_FOR_SCROLL_OFF':                      0xEA08,
    'WAIT_FOR_SCROLL_CHANGE':                   0xEA09,
    'WAIT_FOR_STORAGE_ACTIVITY':                0xEAEE,
    'WAIT_FOR_STORAGE_INACTIVITY':              0xEAEF,
    'HIDE_PAYLOAD':                             0xE9F8,
    'RESTORE_PAYLOAD':                          0xE9F9,
}

# Regexes
define_regex = re.compile(r'^\s*DEFINE\s+.*')
ifdef_regex = re.compile(r'^\s*IF_DEFINED_TRUE\s+.*')
ifnotdef_regex = re.compile(r'^\s*IF_NOT_DEFINED_TRUE\s+.*')
endifdef_regex = re.compile(r'^\s*END_IF_DEFINED\s*$')
elsedef_regex = re.compile(r'^\s*ELSE_DEFINED\s*$')
stringln_regex = re.compile(r'^\s*STRINGLN\s+.*$')
string_regex = re.compile(r'^\s*STRING\s+.*$')
string_inline_regex = re.compile(r'^\s*STRING\s+.*\s+END_STRING$')
stringln_inline_regex = re.compile(r'^\s*STRINGLN\s+.*\s+END_STRING$')
end_string_regex = re.compile(r'^\s*END_STRING(LN)?\s*$')
inline_stringln_regex = re.compile(r'^\s*STRINGLN(_(POWERSHELL|BASH|BATCH|PYTHON|HTML|RUBY|JAVASCRIPT))?\s+.*\s+END_STRINGLN$')
inline_string_regex = re.compile(r'^\s*STRING(_(POWERSHELL|BASH|BATCH|PYTHON|HTML|RUBY|JAVASCRIPT))?\s+.*\s+END_STRING$')
stringln_block_regex = re.compile(r'^\s*STRINGLN(_(POWERSHELL|BASH|BATCH|PYTHON|HTML|RUBY|JAVASCRIPT))?$')
string_block_regex = re.compile(r'^\s*STRING(_(POWERSHELL|BASH|BATCH|PYTHON|HTML|RUBY|JAVASCRIPT))?$')
rem_regex = re.compile(r'^\s*REM.*$')
rem_block_regex = re.compile(r'^\s*REM_BLOCK.*$')
end_rem_block_regex = re.compile(r'^\s*END_REM.*$')
PREPROCESSOR_DISABLED = re.compile(r'^\s*PREPROCESSOR_DISABLED.*$')
USmodifiers_regex = re.compile(r'^\s*(CTRL|CONTROL|SHIFT|ALT|GUI|WINDOWS|CTRL-ALT|COMMAND)$')

# Formatting functions
def dec_to_hex(val):
    val = int(val)
    return f"{val:04x}"

def swap_hex(hex_str):
    hex_str = hex_str.lower()
    if len(hex_str) < 4:
        hex_str = hex_str.zfill(4)
    return hex_str[2:4] + hex_str[0:2]

def hex_encode(h):
    return swap_hex(dec_to_hex(h))

def format_hex(arg):
    return arg.replace('0x', '').replace('0X', '')

def is_var(word):
    return word.startswith("$")

def is_decimal(word):
    return word.isdigit()

def is_hex(word):
    return word.startswith("0x") or word.startswith("0X")

# Variable & Registry logic
def var_exists(word):
    return word in var_map

def allocate_var(word):
    global next_available_var_address
    addr = next_available_var_address
    var_map[word] = addr
    var_values.append('0000')
    next_available_var_address += 1
    return addr

def assign_value(word, val):
    addr = var_map[word]
    var_values[addr] = val
    return addr

def get_var_address(word):
    return var_map[word]

def is_reserved_constant(word):
    return word in reserved_constant_list

def is_reserved_var(word):
    global requires_lang_pack
    if word in reserved_variables_list:
        if word in requires_lang_pack_list:
            requires_lang_pack = True
        return True
    return False

def label_exists(word):
    return word in label_map

def current_register():
    return EVAL_VAR + str(total_eval_registers - free_eval_registers)

def next_register():
    global total_eval_registers, free_eval_registers
    if free_eval_registers == 0:
        total_eval_registers += 1
        free_eval_registers += 1

# Helper function loaders
def get_bytes_for_key(k, fullwidth=False, ducky_lang=None):
    if ducky_lang is None or k not in ducky_lang:
        return None
    c = ducky_lang[k]
    codes = c.split(',')
    if len(codes) < 3:
        return None
    if codes[2] != '00':
        key_codes = [codes[2], codes[0]]
        if fullwidth:
            return key_codes
        if key_codes[1] == '00':
            key_codes.pop()
    else:
        key_codes = [codes[0]]
    return key_codes

def get_args_from_line(line):
    to_consume = [w for w in line.split(' ') if w]
    if not to_consume:
        return None
    to_consume.pop(0) # command
    if to_consume:
        return ' '.join(to_consume)
    return None

def get_cmd_from_line(line):
    to_consume = [w for w in line.split(' ') if w]
    if not to_consume:
        return ""
    return to_consume[0]

def append_hex_string_array(hexStringArray):
    if hexStringArray is None:
        return
    for hex_str in hexStringArray:
        if len(hex_str) > 2:
            oBB.append(hex_str[0:2].lower())
            oBB.append(hex_str[2:4].lower())
        else:
            oBB.append(hex_str.lower())

def do_delay_check():
    if not delay_override and default_delay > 0:
        return build_delay_byte_tab(default_delay)
    return None

def build_delay_byte_tab(d):
    tmp_tab = []
    while d > 0:
        tmp_tab.append('00')
        if d > 255:
            tmp_tab.append('ff')
            d -= 255
        else:
            tmp_tab.append(f"{d:02x}")
            d = 0
    return tmp_tab

def contains_function_call(line):
    return re.search(r'[\w]+\(\)', line) is not None

# String blocks
def handle_rem_block(line):
    global INSIDE_REM_BLOCK
    INSIDE_REM_BLOCK = True
    return None

def handle_end_rem_block(line):
    global INSIDE_REM_BLOCK
    INSIDE_REM_BLOCK = False
    return None

def handle_string_ln_block(line):
    global block_started_at, string_block_indentation_level, INSIDE_STRINGLN_BLOCK
    block_started_at = current_instruction_index
    string_block_indentation_level = 1
    while line.startswith("    "):
        string_block_indentation_level += 1
        line = line[4:]
    INSIDE_STRINGLN_BLOCK = True
    return None

def handle_string_block(line):
    global block_started_at, string_block_indentation_level, preserve_leading_space, INSIDE_STRING_BLOCK
    block_started_at = current_instruction_index
    preserve_leading_space = False
    string_block_indentation_level = 1
    if "python" in line.lower():
        preserve_leading_space = True
        while line.startswith("    "):
            string_block_indentation_level += 1
            line = line[4:]
    INSIDE_STRING_BLOCK = True
    return None

def handle_end_string_block(line):
    global INSIDE_STRINGLN_BLOCK, INSIDE_STRING_BLOCK, string_block_indentation_level
    INSIDE_STRINGLN_BLOCK = False
    INSIDE_STRING_BLOCK = False
    string_block_indentation_level = 1
    return None

def handle_inline_string(line, ducky_lang=None):
    return handle_string(line.replace(" END_STRING", "", 1), ducky_lang=ducky_lang)

def handle_inline_stringln(line, ducky_lang=None):
    return handle_stringln(line.replace(" END_STRINGLN", "", 1).replace(" END_STRING", "", 1), ducky_lang=ducky_lang)

def handle_string(line, ducky_lang=None):
    line = line.lstrip()
    args = get_args_from_line(line)
    tmp = []
    if args is None or len(args) == 0:
        raise ValueError("Nothing to inject")
    for ch in args:
        ba = get_bytes_for_key(ch, ducky_lang=ducky_lang)
        if ba is None:
            # warn / fallback: append 00 00
            tmp.extend(['00', '00'])
        elif len(ba) > 1 and len(ba) < 3:
            tmp.append(ba[0])
            tmp.append(ba[1])
        else:
            tmp.append(ba[0])
            tmp.append('00')
    return tmp

def handle_stringln(line, ducky_lang=None):
    r = handle_string(line.replace("STRINGLN", "STRING", 1), ducky_lang=ducky_lang)
    r.extend(handle_enter("ENTER", ducky_lang=ducky_lang))
    return r

# Function definitions
def handle_function_def(line):
    global defining_function
    if defining_function:
        raise ValueError("Nested functions not allowed")
    defining_function = True
    parts = [w for w in line.strip().split(" ") if w]
    name = parts[1].replace('()', '').strip()
    resp = handle_if("IF FALSE THEN")
    handle_label(f"LABEL function_{name}")
    return resp

def handle_end_function(line):
    global defining_function, return_defined
    if not defining_function:
        raise ValueError("Unexpected function end")
    defining_function = False
    if not return_defined:
        handle_function_call_return("RETURN 0")
    return_defined = False
    return handle_end_if("END_IF")

def handle_function_call(line):
    og_label = line.strip()
    label = og_label.replace('()', '').replace(' ', '')
    label = f"function_{label}"
    if not label_exists(label):
        raise ValueError(f"FUNCTION {og_label} called before it is defined or doesn't exist")
    
    addr = label_map[label] + 6
    hex_addr = dec_to_hex(addr // 2)
    append_hex_string_array(["F7", "F7", hex_addr])
    return None

def handle_function_call_return(line):
    global return_defined
    return_defined = True
    if line.strip() == "RETURN":
        line = 'VAR $_f_ret = 0'
    else:
        line = line.replace('RETURN ', 'VAR $_f_ret = ', 1)
    append_hex_string_array(handle_assignment(line))
    append_hex_string_array(["FD", "FD"])
    return None

# Flow control
def handle_future_address_backfill():
    awaiting_future_addr_assignment[-1].append((len(oBB) - 2, current_line, current_instruction_index))

def close_if_chain():
    if awaiting_future_addr_assignment:
        elem = awaiting_future_addr_assignment.pop()
        for post_process_addr, line_text, inst_idx in elem:
            dec_dest_addr = len(oBB) // 2
            hex_dest_addr = dec_to_hex(dec_dest_addr)
            oBB[post_process_addr] = hex_dest_addr[0:2]
            oBB[post_process_addr + 1] = hex_dest_addr[2:4]

def handle_if(line, chain=False, ducky_lang=None):
    global next_available_block_address, current_block, total_eval_registers, free_eval_registers
    next_available_block_address += 1
    current_block = next_available_block_address
    
    if not chain:
        awaiting_future_addr_assignment.append([])
        
    stack = []
    output_stack = []
    lp_count = 0
    rp_count = 0
    lexemes = [w for w in line.strip().split(" ") if w]
    
    for word in lexemes:
        if word == "IF":
            pass
        elif contains_function_call(word):
            next_register()
            return_reg = current_register()
            stack.append(return_reg)
            handle_function_call(word)
            append_hex_string_array(handle_assignment(f"{return_reg} = $_f_ret", ducky_lang=ducky_lang))
            free_eval_registers -= 1
        elif is_reserved_constant(word):
            stack.append(word)
        elif is_reserved_var(word):
            stack.append(word)
        elif is_var(word):
            stack.append(word)
        elif word in ["THEN", "{"]:
            pass
        elif word == "(":
            lp_count += 1
        elif word == ")":
            rp_count += 1
            temp = []
            if len(stack) > 1:
                next_register()
                var2 = stack.pop()
                op = stack.pop()
                var1 = stack.pop()
                stack.append(current_register())
                
                temp.append("=")
                temp.append(current_register())
                temp.append(var1)
                temp.append(var2)
                temp.append(op)
            elif len(stack) == 0:
                raise ValueError("Empty expression")
            else:
                next_register()
                var1 = stack.pop()
                stack.append(current_register())
                
                temp.append("=")
                temp.append(current_register())
                temp.append(var1)
                temp.append("0000")
            free_eval_registers -= 1
            output_stack.append(temp)
        elif word in operator_keys:
            stack.append(word)
        elif word.isdigit():
            stack.append(word)
        elif is_hex(word):
            stack.append(word)
        else:
            raise ValueError(f"Unexpected Symbol: {word}")
            
    if lp_count != rp_count:
        raise ValueError(f"Mismatched ( ): {lp_count} '(' vs {rp_count} ')'")
        
    if len(stack) == 1:
        output_stack.append(['IF', stack[0]])
    else:
        output_stack.append(['IF', EVAL_VAR + str(total_eval_registers - 1)])
        
    # Encode
    encoded_lines = []
    for entry in output_stack:
        encoding = []
        for word in entry:
            if word == '0000':
                encoding.append('0000')
            elif word == "IF":
                encoding.append(hex_encode(0xEFEF))
            elif is_reserved_var(word):
                encoding.append(hex_encode(reserved_variables[word]))
            elif is_reserved_constant(word):
                encoding.append(hex_encode(reserved_constants[word]))
            elif word in operator_keys:
                encoding.append(hex_encode(operator_map[word]))
            elif is_var(word):
                if var_exists(word):
                    encoding.append(dec_to_hex(get_var_address(word)))
                else:
                    if word.startswith(EVAL_VAR):
                        encoding.append(dec_to_hex(allocate_var(word)))
                        assign_value(word, '0000')
                    else:
                        raise ValueError(f"Unknown Variable: {word}")
            elif word == "=":
                encoding.append(hex_encode(0x01E8))
            elif is_hex(word):
                if var_exists(word):
                    encoding.append(dec_to_hex(get_var_address(word)))
                else:
                    encoding.append(dec_to_hex(allocate_var(word)))
                    assign_value(word, swap_hex(format_hex(word)))
            elif word.isdigit():
                if var_exists(word):
                    encoding.append(dec_to_hex(get_var_address(word)))
                else:
                    encoding.append(dec_to_hex(allocate_var(word)))
                    assign_value(word, hex_encode(int(word)))
        encoded_lines.append(encoding)
        
    # Add block ID to the last encoded line
    encoded_lines[-1].append(dec_to_hex(current_block))
    
    # Flatten to bytes list
    el = []
    for line_enc in encoded_lines:
        for val in line_enc:
            el.append(val[0:2])
            el.append(val[2:4])
            
    block_stack.append(current_block)
    return el

def handle_end_if(line, close_chain=True):
    block_to_close = block_stack.pop()
    bn = dec_to_hex(block_to_close)
    append_hex_string_array(['1F', 'F4', bn])
    if close_chain:
        close_if_chain()
    return None

def handle_else_if(line, ducky_lang=None):
    append_hex_string_array(['F8', 'F8', 'XX', 'XX'])
    handle_future_address_backfill()
    handle_end_if("END_IF", close_chain=False)
    converted_to_if = line.replace('ELSE ', '', 1)
    return handle_if(converted_to_if, chain=True, ducky_lang=ducky_lang)

def handle_else(line, ducky_lang=None):
    append_hex_string_array(['F8', 'F8', 'XX', 'XX'])
    handle_future_address_backfill()
    handle_end_if("END_IF", close_chain=False)
    return handle_if("IF TRUE THEN", chain=True, ducky_lang=ducky_lang)

# While loops
def handle_while(line, ducky_lang=None):
    global next_available_block_address
    handle_label(f"LABEL generated_while_{next_available_block_address + 1}")
    converted_to_if = line.replace('WHILE', 'IF', 1)
    return handle_if(converted_to_if, ducky_lang=ducky_lang)

def handle_end_while(line):
    block_to_close = block_stack[-1]
    handle_goto(f"GOTO generated_while_{block_to_close}")
    return handle_end_if("END_IF")

# Labels & Goto
def handle_label(line):
    label = line.replace('LABEL ', '', 1).strip()
    if label_exists(label):
        raise ValueError(f"Duplicate Definition: {label}")
        
    label_map[label] = len(oBB)
    
    # Fill in gotos awaiting this label
    for lbl in [label, f"function_{label}"]:
        if lbl in goto_awaiting_future_label:
            addr_list = goto_awaiting_future_label[lbl]
            if not isinstance(addr_list, list):
                addr_list = [addr_list]
            for addr_to_fill_in in addr_list:
                dec_addr = len(oBB) // 2
                hex_addr = dec_to_hex(dec_addr)
                oBB[addr_to_fill_in] = hex_addr[0:2]
                oBB[addr_to_fill_in + 1] = hex_addr[2:4]
            del goto_awaiting_future_label[lbl]
    return None

def handle_goto(line):
    label = line.replace('GOTO ', '', 1).strip()
    if not label_exists(label):
        append_hex_string_array(["F8", "F8", "XX", "XX"])
        addr = len(oBB) - 2
        if label not in goto_awaiting_future_label:
            goto_awaiting_future_label[label] = []
        goto_awaiting_future_label[label].append(addr)
        return None
    
    addr = label_map[label]
    hex_addr = dec_to_hex(addr // 2)
    append_hex_string_array(["F8", "F8", hex_addr])
    return None

# Assignment
def handle_assignment(line, ducky_lang=None):
    global total_eval_registers, free_eval_registers
    lexemes = [w for w in line.strip().split(" ") if w]
    is_declaration = False
    
    stack = []
    output_stack = []
    lp_count = 0
    rp_count = 0
    
    for word in lexemes:
        if word == "VAR":
            is_declaration = True
        elif contains_function_call(word):
            next_register()
            return_reg = current_register()
            stack.append(return_reg)
            handle_function_call(word)
            append_hex_string_array(handle_assignment(f"{return_reg} = $_f_ret", ducky_lang=ducky_lang))
            free_eval_registers -= 1
        elif word == "=":
            stack.insert(0, word)
        elif is_reserved_constant(word):
            stack.append(word)
        elif is_reserved_var(word):
            stack.append(word)
        elif is_var(word):
            stack.append(word)
        elif word == "(":
            lp_count += 1
        elif word == ")":
            rp_count += 1
            temp = []
            if len(stack) > 3:
                next_register()
                var2 = stack.pop()
                op = stack.pop()
                var1 = stack.pop()
                stack.append(current_register())
                
                temp.append("=")
                temp.append(current_register())
                temp.append(var1)
                temp.append(var2)
                temp.append(op)
            elif len(stack) == 0:
                raise ValueError("Empty expression")
            else:
                next_register()
                var1 = stack.pop()
                stack.append(current_register())
                
                temp.append("=")
                temp.append(current_register())
                temp.append(var1)
            free_eval_registers -= 1
            output_stack.append(temp)
        elif word in operator_keys:
            stack.append(word)
        elif word.isdigit():
            stack.append(word)
        elif is_hex(word):
            stack.append(word)
        else:
            raise ValueError(f"Unexpected Symbol: {word}")
            
    if lp_count != rp_count:
        raise ValueError(f"Mismatched ( ): {lp_count} '(' vs {rp_count} ')'")
        
    output_stack.append(stack)
    
    # Encode
    encoded_lines = []
    for entry in output_stack:
        encoding = []
        for word in entry:
            if is_reserved_var(word):
                encoding.append(hex_encode(reserved_variables[word]))
            elif is_reserved_constant(word):
                encoding.append(hex_encode(reserved_constants[word]))
            elif word in operator_keys:
                encoding.append(hex_encode(operator_map[word]))
            elif is_var(word):
                if var_exists(word):
                    encoding.append(dec_to_hex(get_var_address(word)))
                else:
                    encoding.append(dec_to_hex(allocate_var(word)))
                    assign_value(word, '0000')
            elif word == "=":
                encoding.append(hex_encode(0x01E8))
            elif is_hex(word):
                if var_exists(word):
                    encoding.append(dec_to_hex(get_var_address(word)))
                else:
                    encoding.append(dec_to_hex(allocate_var(word)))
                    assign_value(word, swap_hex(format_hex(word)))
            elif word.isdigit():
                if var_exists(word):
                    encoding.append(dec_to_hex(get_var_address(word)))
                else:
                    encoding.append(dec_to_hex(allocate_var(word)))
                    assign_value(word, hex_encode(int(word)))
        encoded_lines.append(encoding)
        
    last_line = output_stack[-1]
    if last_line:
        last_token = last_line[-1]
        if last_token not in operator_keys:
            encoded_lines.append(['0000'])
            
    el = []
    for line_enc in encoded_lines:
        for val in line_enc:
            el.append(val[0:2])
            el.append(val[2:4])
    return el

def create_variable(label, val):
    if not var_exists(label):
        addr = allocate_var(label)
        assign_value(label, val)
    else:
        addr = get_var_address(label)
    return addr

# Key & Button handlers
def handle_enter(line, ducky_lang=None):
    enter_bytes = get_bytes_for_key("ENTER", ducky_lang=ducky_lang)
    enter_hex = enter_bytes[0] if enter_bytes else "28"
    return [enter_hex, '00']

def handle_delay(line):
    global delay_override
    delay_override = True
    args = get_args_from_line(line)
    if not args:
        raise ValueError("Delay value missing")
    delay = int(args)
    return build_delay_byte_tab(delay)

def handle_default_delay(line):
    global default_delay, delay_override
    args = get_args_from_line(line)
    if not args:
        raise ValueError("Default delay value missing")
    default_delay = int(args)
    delay_override = True
    return None

def handle_restore_lock_state(line):
    append_hex_string_array([hex_encode(builtins_map["RESTORE_HOST_KEYBOARD_LOCK_STATE"])])
    append_hex_string_array(["0000", "0000", "0000"])
    return None

def handle_string_delay(line, ducky_lang=None):
    args = get_args_from_line(line)
    if not args:
        return []
    arg_split = args.split(" ")
    delay_arg = int(arg_split.pop(0))
    inj_string = " ".join(arg_split)
    
    tmp = []
    for ch in inj_string:
        ba = get_bytes_for_key(ch, ducky_lang=ducky_lang)
        if ba is None:
            tmp.extend(['00', '00'])
        elif len(ba) > 1 and len(ba) < 3:
            tmp.append(ba[0])
            tmp.append(ba[1])
        else:
            tmp.append(ba[0])
            tmp.append('00')
        
        counter = delay_arg
        while counter > 0:
            tmp.append('00')
            if counter > 255:
                tmp.append('ff')
                counter -= 255
            else:
                tmp.append(f"{counter:02x}")
                counter = 0
    return tmp

def handle_random_lower_letter(line):
    global requires_lang_pack
    requires_lang_pack = True
    append_hex_string_array([hex_encode(0xE9E9)])
    append_hex_string_array([hex_encode(0x42FA)])

def handle_random_upper_letter(line):
    global requires_lang_pack
    requires_lang_pack = True
    append_hex_string_array([hex_encode(0xE9E9)])
    append_hex_string_array([hex_encode(0x42FB)])

def handle_random_letter(line):
    global requires_lang_pack
    requires_lang_pack = True
    append_hex_string_array([hex_encode(0xE9E9)])
    append_hex_string_array([hex_encode(0x42FF)])

def handle_random_number(line):
    global requires_lang_pack
    requires_lang_pack = True
    append_hex_string_array([hex_encode(0xE9E9)])
    append_hex_string_array([hex_encode(0x42FC)])

def handle_random_special(line):
    global requires_lang_pack
    requires_lang_pack = True
    append_hex_string_array([hex_encode(0xE9E9)])
    append_hex_string_array([hex_encode(0x42FD)])

def handle_random_char(line):
    global requires_lang_pack
    requires_lang_pack = True
    append_hex_string_array([hex_encode(0xE9E9)])
    append_hex_string_array([hex_encode(0x42FE)])

def handle_breakpoint(line, ducky_lang=None):
    append_hex_string_array(handle_enter("ENTER"))
    append_hex_string_array(handle_string("STRING BREAKPOINT ", ducky_lang=ducky_lang))
    handle_inject_breakpoint_line_number(line, ducky_lang=ducky_lang)
    append_hex_string_array([hex_encode(builtins_map["LED_R"])])
    append_hex_string_array([hex_encode(builtins_map["WAIT_FOR_BUTTON_PRESS"])])
    append_hex_string_array([hex_encode(builtins_map["LED_G"])])
    append_hex_string_array(handle_enter("ENTER"))

def handle_inject_breakpoint_line_number(line, ducky_lang=None):
    linestr = str(max(1, current_instruction_index - 1))
    for char in linestr:
        lookup_and_append(char, ducky_lang=ducky_lang)
    lookup_and_append(';', ducky_lang=ducky_lang)
    return handle_delay("DELAY 800")

def handle_key_hold(line, ducky_lang=None):
    append_hex_string_array([hex_encode(builtins_map["KEY_HOLD"])])
    l = line.replace('HOLD ', '', 1).strip()
    lookup_and_append(l, ducky_lang=ducky_lang)
    if USmodifiers_regex.match(l):
        oBB[-1], oBB[-2] = oBB[-2], oBB[-1]

def handle_repeat(line, ducky_lang=None):
    parts = [w for w in line.strip().split(" ") if w]
    parts.pop(0) # remove REPEAT
    if not parts:
        raise ValueError("REPEAT missing count")
    count_str = parts.pop(0)
    try:
        count = int(count_str)
    except ValueError:
        raise ValueError(f"REPEAT count invalid: {count_str}")
    cmd_line = ' '.join(parts)
    for _ in range(count):
        delegate_ds_command(cmd_line, ducky_lang=ducky_lang)
        delay_to_add = do_delay_check()
        if delay_to_add is not None:
            append_hex_string_array(delay_to_add)

def handle_key_release(line, ducky_lang=None):
    l = line.replace('RELEASE ', '', 1).strip()
    if l == '':
        append_hex_string_array(['00', '00'])
    else:
        append_hex_string_array([hex_encode(builtins_map["KEY_RELEASE"])])
        lookup_and_append(l, ducky_lang=ducky_lang)
        if re.search(r'(?:[\s]|^)(CTRL|CONTROL|SHIFT|ALT|GUI|WINDOWS|CTRL\-ALT|COMMAND|COMMAND\-CTRL|COMMAND\-CTRL\-SHIFT|COMMAND\-OPTION|COMMAND\-OPTION\-SHIFT)(?=[\s]|$)', l):
            oBB[-1], oBB[-2] = oBB[-2], oBB[-1]

def handle_inject_mod(line):
    append_hex_string_array([hex_encode(0xE9E6)])
    return None

def handle_inject_mod_param(line, ducky_lang=None):
    append_hex_string_array([hex_encode(0xE9E6)])
    return handle_modifier(line.replace("INJECT_MOD ", "", 1), warn=False, ducky_lang=ducky_lang)

def handle_inject_key(line):
    append_hex_string_array([hex_encode(0xE8E9)])
    arg = line.replace("INJECT ", "", 1).replace("0x", "").strip()
    append_hex_string_array([arg])
    return None

def handle_inject_keycode(line):
    append_hex_string_array([hex_encode(0xE8E9)])
    arg = line.replace("KEYCODE ", "", 1).replace("0x", "").strip()
    append_hex_string_array([arg])
    return None

def handle_inject_var(line):
    word = line.replace("INJECT_VAR ", "", 1).strip()
    append_hex_string_array([hex_encode(0xE9E9)])
    if var_exists(word):
        append_hex_string_array([dec_to_hex(get_var_address(word))])
    else:
        raise ValueError(f"Variable {word} does not exist")
    return None

def handle_delay_var(line):
    append_hex_string_array([hex_encode(0xE9E7)])
    word = line.replace("DELAY ", "", 1).strip()
    if var_exists(word):
        append_hex_string_array([dec_to_hex(get_var_address(word))])
    elif word == "$_RANDOM_INT":
        append_hex_string_array([hex_encode(reserved_variables[word])])
    else:
        raise ValueError(f"Variable {word} does not exist")
    return None

def handle_exfil_var(line):
    word = line.replace("EXFIL ", "", 1).strip()
    append_hex_string_array([hex_encode(0xE9F6)])
    if var_exists(word):
        append_hex_string_array([dec_to_hex(get_var_address(word))])
    else:
        raise ValueError(f"Variable {word} does not exist")
    return None

def handle_key_down(line):
    append_hex_string_array([hex_encode(0xEA0A)])
    arg = line.replace("KEY_DOWN ", "", 1).replace("0x", "").strip()
    append_hex_string_array([arg])
    return None

def handle_key_up(line):
    append_hex_string_array([hex_encode(0xEA0B)])
    arg = line.replace("KEY_UP ", "", 1).replace("0x", "").strip()
    append_hex_string_array([arg])
    return None

def handle_mod_down(line):
    append_hex_string_array([hex_encode(0xEA0C)])
    arg = line.replace("MOD_DOWN ", "", 1).replace("0x", "").strip()
    append_hex_string_array([arg])
    return None

def handle_mod_key_down(line):
    append_hex_string_array([hex_encode(0xEA0D)])
    arg = line.replace("MOD_KEY_DOWN ", "", 1).replace("0x", "").strip()
    append_hex_string_array([arg])
    return None

def handle_mod_key_up(line):
    append_hex_string_array([hex_encode(0xEA0E)])
    arg = line.replace("MOD_KEY_UP ", "", 1).replace("0x", "").strip()
    append_hex_string_array([arg])
    return None

def handle_mod_up(line):
    append_hex_string_array([hex_encode(0xEA0F)])
    arg = line.replace("MOD_UP ", "", 1).replace("0x", "").strip()
    append_hex_string_array([arg])
    return None

def handle_button_def(line):
    global defining_button, button_block_counter, defining_button_at
    defining_button += 1
    button_block_counter += 1
    defining_button_at = current_instruction_index
    button_block_stack.append(button_block_counter)
    return ["ea", "ee", dec_to_hex(button_block_counter)]

def handle_end_button_def(line):
    global defining_button
    if defining_button == 0:
        raise ValueError("Unexpected END_BUTTON")
    defining_button -= 1
    addr = button_block_stack.pop()
    return ["eb", "f4", dec_to_hex(addr)]

def handle_modifier(line, warn=True, ducky_lang=None):
    c = get_cmd_from_line(line)
    args = get_args_from_line(line)
    key_hex = None
    if args and len(args) > 0:
        key = args[0]
        key_hex = get_bytes_for_key(key, ducky_lang=ducky_lang)
    mod_hex = get_bytes_for_key(c, ducky_lang=ducky_lang)
    if mod_hex is None:
        mod_hex = ["00"]
    if key_hex is None:
        key_hex = ["00"]
    return [key_hex[0], mod_hex[0]]

def find_key_by_value(ducky_lang, value):
    for key, val in ducky_lang.items():
        if val == value:
            return key
    return None

def lookup_and_append(command, recursive=False, dontsplit=False, ducky_lang=None):
    if not dontsplit:
        splitcommand = [w for w in command.split(" ") if w]
        buffer = []
        mod_buffer = []
        if len(splitcommand) > 1:
            for i in range(len(splitcommand)):
                if splitcommand[i] == "-":
                    continue
                keycodes = get_bytes_for_key(splitcommand[i], fullwidth=True, ducky_lang=ducky_lang)
                if keycodes is None:
                    return lookup_and_append(command, recursive=True, dontsplit=True, ducky_lang=ducky_lang)
                
                has_mod = False
                if len(keycodes) > 1:
                    if keycodes[1] != "00":
                        has_mod = True
                else:
                    has_mod = True
                
                if has_mod:
                    if len(keycodes) > 1 and keycodes[0] != '00':
                        modifier_key = find_key_by_value(ducky_lang, f"{keycodes[1]},00,00")
                        raise ValueError(f"STRICT COMBOS - PREVENTING IMPLICIT MODIFIER COMBINATION: {splitcommand[i]} already contains {modifier_key}")
                    res = lookup_and_append(splitcommand[i], recursive=True, ducky_lang=ducky_lang)
                    if res:
                        mod_buffer.append(res)
                else:
                    res = lookup_and_append(splitcommand[i], recursive=True, ducky_lang=ducky_lang)
                    if res:
                        buffer.append(res)
            
            key_sum = sum(int(b[0], 16) for b in buffer)
            mod_sum = sum(int(m[0], 16) for m in mod_buffer)
            
            key_hex = f"{key_sum:02x}"
            mod_hex = f"{mod_sum:02x}"
            
            if recursive:
                return [key_hex, mod_hex]
            
            append_hex_string_array([key_hex, mod_hex])
            return True
        elif len(splitcommand) > 0:
            command = splitcommand[0]

    key_codes = get_bytes_for_key(command, ducky_lang=ducky_lang)
    if key_codes is None:
        if len(command) == 1:
            raise ValueError(f"Character {command} not found in language map")
        return lookup_and_append(command[0], recursive=recursive, ducky_lang=ducky_lang)

    if recursive:
        return key_codes

    if len(key_codes) > 1 and len(key_codes) < 3:
        append_hex_string_array(key_codes)
    else:
        append_hex_string_array(key_codes)
        append_hex_string_array(['00'])
    return True

# Attackmode compiler
def handle_attackmode(line):
    global var_values
    lexemes = [w for w in line.strip().split(" ") if w]
    encoded_line = []
    mode_defined = False
    mode_hex = 0x0000
    vid_defined = False
    pid_defined = False
    man_defined = False
    prod_defined = False
    serial_defined = False
    
    i = 0
    quit_parsing = False
    while i < len(lexemes) and not quit_parsing:
        word = lexemes[i]
        if word == "ATTACKMODE":
            i += 1
            if i >= len(lexemes):
                raise ValueError("ATTACKMODE missing mode")
            mode = lexemes[i]
            if mode == "OFF":
                mode_defined = True
                mode_hex = 0xF0F0
                encoded_line.append(mode_hex)
                quit_parsing = True
            elif mode == "HID":
                mode_defined = True
                if i + 1 < len(lexemes) and lexemes[i+1] == "STORAGE":
                    mode_hex = 0xF3F3
                    encoded_line.append(mode_hex)
                    i += 1
                else:
                    mode_hex = 0xF1F1
                    encoded_line.append(mode_hex)
            elif mode == "STORAGE":
                mode_defined = True
                if i + 1 < len(lexemes) and lexemes[i+1] == "HID":
                    mode_hex = 0xF3F3
                    encoded_line.append(mode_hex)
                    i += 1
                else:
                    mode_hex = 0xF2F2
                    encoded_line.append(mode_hex)
            else:
                raise ValueError(f"Invalid Attackmode Mode: {mode}")
        elif word == "VID_RANDOM":
            encoded_line.append(0xF5F5)
            encoded_line.append(0xF342) # $_RANDOM_UINT16
            vid_defined = True
        elif word.startswith("VID_$"):
            encoded_line.append(0xF5F5)
            vid_defined = True
            var_name = word[4:]
            if var_exists(var_name):
                encoded_line.append(dec_to_hex(get_var_address(var_name)))
            else:
                raise ValueError(f"VID VARIABLE {var_name} does not exist")
        elif word.startswith("VID_"):
            arg = word[4:]
            encoded_line.append(0xF5F5)
            encoded_line.append(create_variable(format_hex(arg), format_hex(arg)))
            vid_defined = True
        elif word == "PID_RANDOM":
            encoded_line.append(0xF6F6)
            encoded_line.append(0xF342) # $_RANDOM_UINT16
            pid_defined = True
        elif word.startswith("PID_$"):
            encoded_line.append(0xF6F6)
            pid_defined = True
            var_name = word[4:]
            if var_exists(var_name):
                encoded_line.append(dec_to_hex(get_var_address(var_name)))
            else:
                raise ValueError(f"PID VARIABLE {var_name} does not exist")
        elif word.startswith("PID_"):
            arg = word[4:]
            encoded_line.append(0xF6F6)
            encoded_line.append(create_variable(format_hex(arg), format_hex(arg)))
            pid_defined = True
        elif word == "MAN_RANDOM":
            encoded_line.append(0xF9F9)
            for _ in range(12):
                encoded_line.append(0xF542) # $_RANDOM_ASCII_UPPER_LETTER
            encoded_line.append(0xF9F9)
            man_defined = True
        elif word.startswith("MAN_"):
            arg = word[4:]
            if not arg or len(arg) > 32:
                raise ValueError(f"Invalid Manufacturer string: {word}")
            encoded_line.append(0xF9F9)
            for c in arg:
                encoded_line.append(
                    create_variable(
                        f"__CHAR_{hex_encode(ord(c))}",
                        hex_encode(ord(c))
                    )
                )
            encoded_line.append(0xF9F9)
            man_defined = True
        elif word == "PROD_RANDOM":
            encoded_line.append(0xFAFA)
            for _ in range(12):
                encoded_line.append(0xF542) # $_RANDOM_ASCII_UPPER_LETTER
            encoded_line.append(0xFAFA)
            prod_defined = True
        elif word.startswith("PROD_"):
            arg = word[5:]
            if not arg or len(arg) > 32:
                raise ValueError(f"Invalid Product string: {word}")
            encoded_line.append(0xFAFA)
            for c in arg:
                encoded_line.append(
                    create_variable(
                        f"__CHAR_{hex_encode(ord(c))}",
                        hex_encode(ord(c))
                    )
                )
            encoded_line.append(0xFAFA)
            prod_defined = True
        elif word == "SERIAL_RANDOM":
            encoded_line.append(0xFBFB)
            for _ in range(12):
                encoded_line.append(0xF742) # $_RANDOM_ASCII_NUMBER
            encoded_line.append(0xFBFB)
            serial_defined = True
        elif word.startswith("SERIAL_"):
            arg = word[7:]
            if not arg or len(arg) > 12 or not arg.isdigit():
                raise ValueError(f"Invalid Serial: {word}")
            encoded_line.append(0xFBFB)
            for c in arg:
                encoded_line.append(
                    create_variable(
                        f"__CHAR_{hex_encode(ord(c))}",
                        hex_encode(ord(c))
                    )
                )
            encoded_line.append(0xFBFB)
            serial_defined = True
        else:
            raise ValueError(f"Unexpected Symbol: {word}")
        i += 1

    if not mode_defined:
        raise ValueError("ATTACKMODE missing or invalid mode")
    if vid_defined or pid_defined:
        if not vid_defined or not pid_defined:
            raise ValueError("VID + PID must both be defined")
    if man_defined or serial_defined or prod_defined:
        if not man_defined or not serial_defined or not prod_defined:
            raise ValueError("MAN + SERIAL + PROD must all be defined")
            
    encoded_line.append(mode_hex)
    
    el = []
    for val in encoded_line:
        if isinstance(val, int):
            b = dec_to_hex(val)
        else:
            b = val
        el.append(b[0:2])
        el.append(b[2:4])
    return el

# Ducky compiler syntax list
syntax_handlers = [
    (PREPROCESSOR_DISABLED, lambda line, ducky_lang=None: None),
    (rem_block_regex, lambda line, ducky_lang=None: handle_rem_block(line)),
    (end_rem_block_regex, lambda line, ducky_lang=None: handle_end_rem_block(line)),
    (rem_regex, lambda line, ducky_lang=None: None),
    (re.compile(r'^\/\/ .*$'), lambda line, ducky_lang=None: None),
    (re.compile(r'^\s*\n .*$'), lambda line, ducky_lang=None: None),
    
    (define_regex, lambda line, ducky_lang=None: None),
    (ifdef_regex, lambda line, ducky_lang=None: None),
    (ifnotdef_regex, lambda line, ducky_lang=None: None),
    (elsedef_regex, lambda line, ducky_lang=None: None),
    (endifdef_regex, lambda line, ducky_lang=None: None),
    
    (re.compile(r'^\s*INJECT_VAR\ \$.*$'), lambda line, ducky_lang=None: handle_inject_var(line)),
    (re.compile(r'^\s*DELAY\ \$.*$'), lambda line, ducky_lang=None: handle_delay_var(line)),
    (re.compile(r'^\s*DELAY .*'), lambda line, ducky_lang=None: handle_delay(line)),
    (end_string_regex, lambda line, ducky_lang=None: handle_end_string_block(line)),
    (stringln_inline_regex, lambda line, ducky_lang=None: handle_inline_stringln(line, ducky_lang=ducky_lang)),
    (string_inline_regex, lambda line, ducky_lang=None: handle_inline_string(line, ducky_lang=ducky_lang)),
    (inline_stringln_regex, lambda line, ducky_lang=None: handle_inline_stringln(line, ducky_lang=ducky_lang)),
    (inline_string_regex, lambda line, ducky_lang=None: handle_inline_string(line, ducky_lang=ducky_lang)),
    (stringln_block_regex, lambda line, ducky_lang=None: handle_string_ln_block(line)),
    (string_block_regex, lambda line, ducky_lang=None: handle_string_block(line)),
    (stringln_regex, lambda line, ducky_lang=None: handle_stringln(line, ducky_lang=ducky_lang)),
    (string_regex, lambda line, ducky_lang=None: handle_string(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*HOLD .*$'), lambda line, ducky_lang=None: handle_key_hold(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*RELEASE .*$'), lambda line, ducky_lang=None: handle_key_release(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*REPEAT (?:[2-9]|\d\d\d*).*$'), lambda line, ducky_lang=None: handle_repeat(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*ENTER.*$'), lambda line, ducky_lang=None: handle_enter(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*DEFAULTDELAY (?:[2-9]|\d\d\d*)$'), lambda line, ducky_lang=None: handle_default_delay(line)),
    (re.compile(r'^\s*DEFAULT_DELAY (?:[2-9]|\d\d\d*)$'), lambda line, ducky_lang=None: handle_default_delay(line)),
    (re.compile(r'^\s*STRINGDELAY (?:[2-9]|\d\d\d*).*$'), lambda line, ducky_lang=None: handle_string_delay(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*STRING_DELAY (?:[2-9]|\d\d\d*).*$'), lambda line, ducky_lang=None: handle_string_delay(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*IF .*', re.IGNORECASE), lambda line, ducky_lang=None: handle_if(line, ducky_lang=ducky_lang)),
    (re.compile(r'^}.*', re.IGNORECASE), lambda line, ducky_lang=None: handle_end_if(line)),
    (re.compile(r'^\s*END_IF.*', re.IGNORECASE), lambda line, ducky_lang=None: handle_end_if(line)),
    (re.compile(r'^\s*ELSE IF .*', re.IGNORECASE), lambda line, ducky_lang=None: handle_else_if(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*ELSE.*', re.IGNORECASE), lambda line, ducky_lang=None: handle_else(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*WHILE .*', re.IGNORECASE), lambda line, ducky_lang=None: handle_while(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*END_WHILE.*'), lambda line, ducky_lang=None: handle_end_while(line)),
    (re.compile(r'^\s*STAGE .*$'), lambda line, ducky_lang=None: None),
    (re.compile(r'^\s*END_STAGE.*'), lambda line, ducky_lang=None: None),
    (re.compile(r'^\s*EXTENSION .*$'), lambda line, ducky_lang=None: None),
    (re.compile(r'^\s*END_EXTENSION.*'), lambda line, ducky_lang=None: None),
    (re.compile(r'^\s*FUNCTION\ .*\(\).*'), lambda line, ducky_lang=None: handle_function_def(line)),
    (re.compile(r'^\s*END_FUNCTION.*'), lambda line, ducky_lang=None: handle_end_function(line)),
    (re.compile(r'^\s*ATTACKMODE .*'), lambda line, ducky_lang=None: handle_attackmode(line)),
    (re.compile(r'^\s*RETURN .*'), lambda line, ducky_lang=None: handle_function_call_return(line)),
    (re.compile(r'\$.*= .*'), lambda line, ducky_lang=None: handle_assignment(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*VAR \$.*= .*'), lambda line, ducky_lang=None: handle_assignment(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*[\w]+\(\)'), lambda line, ducky_lang=None: handle_function_call(line)),
    (re.compile(r'^\s*BUTTON_DEF.*'), lambda line, ducky_lang=None: handle_button_def(line)),
    (re.compile(r'^\s*END_BUTTON.*'), lambda line, ducky_lang=None: handle_end_button_def(line)),
    (re.compile(r'^\s*DEBUGGER_BREAKPOINT.*'), lambda line, ducky_lang=None: handle_breakpoint(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*INJECT_BREAKPOINT_LINE_NUMBER.*'), lambda line, ducky_lang=None: handle_inject_breakpoint_line_number(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*RANDOM_LOWERCASE_LETTER.*'), lambda line, ducky_lang=None: handle_random_lower_letter(line)),
    (re.compile(r'^\s*RANDOM_UPPERCASE_LETTER.*'), lambda line, ducky_lang=None: handle_random_upper_letter(line)),
    (re.compile(r'^\s*RANDOM_NUMBER.*'), lambda line, ducky_lang=None: handle_random_number(line)),
    (re.compile(r'^\s*RANDOM_LETTER.*'), lambda line, ducky_lang=None: handle_random_letter(line)),
    (re.compile(r'^\s*RANDOM_SPECIAL.*'), lambda line, ducky_lang=None: handle_random_special(line)),
    (re.compile(r'^\s*RANDOM_CHAR.*'), lambda line, ducky_lang=None: handle_random_char(line)),
    (re.compile(r'^\s*RESTORE_HOST_KEYBOARD_LOCK_STATE.*'), lambda line, ducky_lang=None: handle_restore_lock_state(line)),
    (re.compile(r'^\s*INJECT_MOD\s*$'), lambda line, ducky_lang=None: handle_inject_mod(line)),
    (re.compile(r'^\s*INJECT_MOD .*$'), lambda line, ducky_lang=None: handle_inject_mod_param(line, ducky_lang=ducky_lang)),
    (re.compile(r'^\s*INJECT .*'), lambda line, ducky_lang=None: handle_inject_key(line)),
    (re.compile(r'^\s*KEYCODE .*$'), lambda line, ducky_lang=None: handle_inject_keycode(line)),
    (re.compile(r'^\s*EXFIL \$.*'), lambda line, ducky_lang=None: handle_exfil_var(line)),
    (re.compile(r'^\s*MOD_KEY_DOWN .*'), lambda line, ducky_lang=None: handle_mod_key_down(line)),
    (re.compile(r'^\s*MOD_KEY_UP .*'), lambda line, ducky_lang=None: handle_mod_key_up(line)),
    (re.compile(r'^\s*MOD_UP .*'), lambda line, ducky_lang=None: handle_mod_up(line)),
    (re.compile(r'^\s*KEY_UP .*'), lambda line, ducky_lang=None: handle_key_up(line)),
    (re.compile(r'^\s*KEY_DOWN .*'), lambda line, ducky_lang=None: handle_key_down(line)),
    (re.compile(r'^\s*MOD_DOWN .*'), lambda line, ducky_lang=None: handle_mod_down(line)),
    (USmodifiers_regex, lambda line, ducky_lang=None: handle_modifier(line, ducky_lang=ducky_lang)),
]

# Lexing and Preprocessing
def split_ds(dS):
    return dS.split('\n')

def sanitize_ds(dS):
    noR = dS.replace('\r', '')
    return noR.replace('”', '"')

def split_syntax_line(line, state):
    global end_string_regex, string_block_regex, stringln_block_regex
    is_injection_string = False
    
    if state["inside_string_block"] or state["inside_stringln_block"]:
        if re.match(end_string_regex, line):
            state["inside_string_block"] = False
            state["inside_stringln_block"] = False
        else:
            is_injection_string = True
    else:
        if (re.match(string_regex, line) or 
            re.match(stringln_regex, line) or 
            re.match(inline_string_regex, line) or 
            re.match(inline_stringln_regex, line)):
            is_injection_string = True
        elif re.match(string_block_regex, line) or re.match(stringln_block_regex, line):
            state["inside_string_block"] = True
            state["inside_stringln_block"] = True
            is_injection_string = True
        else:
            line = line.strip()
            
    l = list(line)
    i = 0
    while i < len(l):
        c = l[i]
        c1 = l[i + 1] if i + 1 < len(l) else ""
        if c in operator_keys or c == '(' or c == ')':
            if (c + c1) in double_operator:
                i += 1
            else:
                if not is_injection_string:
                    l[i] = ' ' + c + ' '
        i += 1
        
    line_joined = "".join(l)
    if is_injection_string:
        return line_joined.split(' ')
    
    words = line_joined.split(' ')
    return [w for w in words if w]

def gather_defines(lines, preprocessor_labels, preprocessor_replacements):
    inside_rem = False
    new_lines = []
    for line in lines:
        stripped = line.strip()
        if re.match(rem_block_regex, stripped):
            inside_rem = True
            new_lines.append(line)
            continue
        elif re.match(end_rem_block_regex, stripped):
            inside_rem = False
            new_lines.append(line)
            continue
            
        if not inside_rem and re.match(define_regex, stripped):
            words = stripped.split(" ")
            label = words[1]
            prefix = f"DEFINE {label} "
            if prefix in stripped:
                replacement = stripped.split(prefix, 1)[1]
            else:
                replacement = ""
            
            if label in preprocessor_labels:
                raise ValueError(f"Duplicate DEFINE Label: {label}")
            preprocessor_labels.append(label)
            preprocessor_replacements.append(replacement)
            
        new_lines.append(line)
    return new_lines

def parse_if_defs(lines, preprocessor_labels, preprocessor_replacements):
    ifdef_stack = []
    new_lines = []
    
    def search_label(label, match):
        for lbl, repl in zip(preprocessor_labels, preprocessor_replacements):
            if label == lbl:
                return repl == match
        return match == "FALSE"
        
    for i, line in enumerate(lines):
        if not line:
            new_lines.append(line)
            continue
            
        stripped = line.strip()
        parts = stripped.split(" ")
        label = parts[1] if len(parts) > 1 else ""
        
        found = False
        if re.match(ifdef_regex, line):
            found = search_label(label, "TRUE")
            ifdef_stack.append((stripped, found))
        elif re.match(ifnotdef_regex, line):
            found = not search_label(label, "FALSE")
            ifdef_stack.append((stripped, found))
        elif re.match(elsedef_regex, stripped):
            if not ifdef_stack:
                raise ValueError(f"No IF_DEFINED_TRUE or IF_NOT_DEFINED_TRUE to negate at line {i+1}")
            m, mode_val = ifdef_stack.pop()
            ifdef_stack.append((m, not mode_val))
        elif re.match(endifdef_regex, stripped):
            if not ifdef_stack:
                raise ValueError(f"Mismatched END_IF_DEFINED at line {i+1}")
            ifdef_stack.pop()
            
        if ifdef_stack:
            inside_enabled_code = True
            for mode, currentval in ifdef_stack:
                if re.match(ifdef_regex, mode):
                    if not currentval:
                        inside_enabled_code = False
                elif re.match(ifnotdef_regex, mode):
                    if currentval:
                        inside_enabled_code = False
                if not inside_enabled_code:
                    break
            if not inside_enabled_code:
                new_lines.append("PREPROCESSOR_DISABLED " + line)
                continue
                
        new_lines.append(line)
    return new_lines

def preprocess_pass(lines, preprocessor_labels, preprocessor_replacements):
    state = {"inside_string_block": False, "inside_stringln_block": False}
    new_lines = []
    for line in lines:
        stripped = line.strip()
        if (re.match(rem_regex, stripped) or 
            re.match(define_regex, stripped) or 
            re.match(ifdef_regex, stripped) or 
            re.match(ifnotdef_regex, stripped) or 
            re.match(elsedef_regex, stripped) or 
            stripped.startswith("PREPROCESSOR_DISABLED")):
            new_lines.append(line)
            continue
            
        words = split_syntax_line(line, state)
        
        for w in range(len(words)):
            for label, replacement in zip(preprocessor_labels, preprocessor_replacements):
                if words[w] == label:
                    words[w] = replacement
                    
        line = " ".join(words)
        
        for label, replacement in zip(preprocessor_labels, preprocessor_replacements):
            if label.startswith('#'):
                while label in line:
                    line = line.replace(label, replacement)
        new_lines.append(line)
    return new_lines

def delegate_ds_command(line, ducky_lang=None):
    global INSIDE_REM_BLOCK, INSIDE_STRING_BLOCK, INSIDE_STRINGLN_BLOCK
    if not line or line.strip() == '':
        return
        
    if INSIDE_REM_BLOCK:
        if re.match(end_rem_block_regex, line):
            handle_end_rem_block(line)
        return
        
    if INSIDE_STRING_BLOCK:
        if re.match(end_string_regex, line):
            handle_end_string_block(line)
        else:
            if not preserve_leading_space:
                line = line.lstrip()
            else:
                for _ in range(string_block_indentation_level - 1):
                    if line.startswith("    "):
                        line = line[4:]
            line = "STRING " + line
            tmp_bytes = handle_string(line, ducky_lang=ducky_lang)
            if tmp_bytes:
                append_hex_string_array(tmp_bytes)
        return

    if INSIDE_STRINGLN_BLOCK:
        if re.match(end_string_regex, line):
            handle_end_string_block(line)
        else:
            for _ in range(string_block_indentation_level - 1):
                if line.startswith("    "):
                    line = line[4:]
            line = "STRINGLN " + line
            tmp_bytes = handle_stringln(line, ducky_lang=ducky_lang)
            if tmp_bytes:
                append_hex_string_array(tmp_bytes)
        return

    f_call = None
    for pattern, handler in syntax_handlers:
        if re.match(pattern, line):
            f_call = handler
            break
            
    if f_call is None:
        stripped = line.strip()
        if stripped in builtins_map:
            append_hex_string_array([hex_encode(builtins_map[stripped])])
        else:
            if lookup_and_append(stripped, ducky_lang=ducky_lang) is None:
                raise ValueError(f"Unrecognized syntax - Key not found in language file: {line}")
    else:
        import inspect
        sig = inspect.signature(f_call)
        if 'ducky_lang' in sig.parameters:
            tmp_bytes = f_call(line, ducky_lang=ducky_lang)
        else:
            tmp_bytes = f_call(line)
            
        if tmp_bytes:
            append_hex_string_array(tmp_bytes)

def generate_lang_pack(ducky_lang=None):
    shift_bytes = get_bytes_for_key("SHIFT", ducky_lang=ducky_lang)
    shift = shift_bytes[0] if shift_bytes else "02"
    to_pack = [
        "a","b","c","d","e","f","g","h","i","j","k","l","m",
        "n","o","p","q","r","s","t","u","v","w","x","y","z","1","2","3",
        "4","5","6","7","8","9","0"
    ]
    create_variable(hex_encode(0xEEEE), hex_encode(0xEEEE))
    for char in to_pack:
        k = get_bytes_for_key(char, fullwidth=True, ducky_lang=ducky_lang)
        if k and len(k) > 1:
            k[1] = shift
        else:
            k = [k[0] if k else "00", shift]
        create_variable(f'LANG_{char}', "".join(k))

def compile_payload(duckyscript, ducky_lang):
    global oBB, current_instruction_index, current_line, default_delay, delay_override
    global total_eval_registers, free_eval_registers, current_block, next_available_block_address
    global block_stack, awaiting_future_addr_assignment, next_available_var_address
    global var_map, var_values, requires_lang_pack, label_map, goto_awaiting_future_label
    global defining_function, button_block_stack, button_block_counter, defining_button, return_defined
    global preprocessor_labels, preprocessor_replacements
    global INSIDE_REM_BLOCK, INSIDE_STRING_BLOCK, INSIDE_STRINGLN_BLOCK
    
    # Reset state
    oBB = []
    current_instruction_index = 0
    current_line = ""
    default_delay = 0
    delay_override = False
    total_eval_registers = 0
    free_eval_registers = 0
    current_block = 0x0000
    next_available_block_address = 0x0000
    block_stack = []
    awaiting_future_addr_assignment = []
    next_available_var_address = 0x0001
    var_map = {}
    var_values = ['VALUES']
    requires_lang_pack = False
    label_map = {}
    goto_awaiting_future_label = {}
    defining_function = False
    button_block_stack = []
    button_block_counter = 0
    defining_button = 0
    return_defined = False
    preprocessor_labels = []
    preprocessor_replacements = []
    INSIDE_REM_BLOCK = False
    INSIDE_STRING_BLOCK = False
    INSIDE_STRINGLN_BLOCK = False
    
    sanitized = sanitize_ds(duckyscript)
    lines = split_ds(sanitized)
    lines = ["SPACE" if l == " " else l for l in lines]
    
    lines = gather_defines(lines, preprocessor_labels, preprocessor_replacements)
    lines = parse_if_defs(lines, preprocessor_labels, preprocessor_replacements)
    lines = preprocess_pass(lines, preprocessor_labels, preprocessor_replacements)
    
    for current_instruction_index in range(len(lines)):
        current_line = lines[current_instruction_index]
        delegate_ds_command(current_line, ducky_lang=ducky_lang)
        
        delay_to_add = do_delay_check()
        if delay_to_add is not None:
            append_hex_string_array(delay_to_add)
            
        free_eval_registers = total_eval_registers
        
    if block_stack:
        raise ValueError("Missing END_*")
    if goto_awaiting_future_label:
        raise ValueError("Misnamed function def or label not found")
    if INSIDE_STRINGLN_BLOCK:
        raise ValueError("Missing END_STRINGLN")
    if INSIDE_STRING_BLOCK:
        raise ValueError("Missing END_STRING")
    if INSIDE_REM_BLOCK:
        raise ValueError("Missing END_REM")
    if defining_button > 0:
        raise ValueError("Missing END_BUTTON")
        
    added_bytes = 0
    if requires_lang_pack:
        generate_lang_pack(ducky_lang=ducky_lang)
        
    if len(var_values) > 1:
        added_bytes += 2
        oBB.insert(0, 'e8e8')
        for val in reversed(var_values[1:]):
            oBB.insert(0, val)
            added_bytes += 1
        oBB.insert(0, 'e8e8')
        
    joined_hex = "".join(oBB)
    ui16 = [joined_hex[i:i+4] for i in range(0, len(joined_hex), 4)]
    
    i = 0
    while i < len(ui16):
        word = ui16[i].lower()
        if word == "f8f8" or word == "f7f7":
            if i + 1 < len(ui16):
                current_addr = ui16[i+1]
                if "xx" in current_addr.lower():
                    raise ValueError("Target address was not backfilled")
                dec_addr = int(current_addr, 16)
                shifted_dec_addr = dec_addr + added_bytes
                shifted_hex_addr = hex_encode(shifted_dec_addr)
                ui16[i+1] = shifted_hex_addr
        i += 1
        
    final_hex = "".join(ui16)
    return bytes.fromhex(final_hex)

def invert_operator_map(op_map):
    inv = {}
    for k, v in op_map.items():
        inv[hex_encode(v)] = k
    return inv

def invert_builtins_map(b_map):
    inv = {}
    for k, v in b_map.items():
        encoded = hex_encode(v)
        if encoded not in inv or len(k) < len(inv[encoded]):
            inv[encoded] = k
    return inv

def invert_reserved_map(r_map):
    inv = {}
    for k, v in r_map.items():
        inv[hex_encode(v)] = k
    return inv

def decompile_payload(binary_data, ducky_lang):
    hex_data = binary_data.hex()
    words = [hex_data[i:i+4] for i in range(0, len(hex_data), 4)]
    
    inv_op_map = invert_operator_map(operator_map)
    inv_builtins = invert_builtins_map(builtins_map)
    inv_reserved_vars = invert_reserved_map(reserved_variables)
    inv_reserved_consts = invert_reserved_map(reserved_constants)
    
    inv_layout = {}
    for k, v in ducky_lang.items():
        if k.startswith('__comment'): continue
        parts = v.split(',')
        if len(parts) >= 3:
            mod = parts[0].zfill(2)
            kc = parts[2].zfill(2)
            key_id = f"{mod},00,{kc}".lower()
            if key_id not in inv_layout or len(k) < len(inv_layout[key_id]) or k == " ":
                if k == "SPACE" and "00,00,2c" in inv_layout: continue
                inv_layout[key_id] = k

    def decode_word(w):
        kc = w[0:2]
        mod = w[2:4]
        key_id = f"{mod},00,{kc}".lower()
        if key_id in inv_layout:
            return inv_layout[key_id]
        elif mod != '00':
            m_names = []
            m_val = int(mod, 16)
            if m_val & 1: m_names.append("CTRL")
            if m_val & 4: m_names.append("ALT")
            if m_val & 2: m_names.append("SHIFT")
            if m_val & 8: m_names.append("GUI")
            base_key_id = f"00,00,{kc}".lower()
            base_name = inv_layout.get(base_key_id, f"KEYCODE 0x{kc}")
            return "-".join(m_names) + " " + base_name
        return f"KEYCODE 0x{kc}"

    variables = []
    idx = 0
    
    if idx < len(words) and words[idx] == 'e8e8':
        idx += 1
        var_values = []
        while idx < len(words) and words[idx] != 'e8e8':
            var_values.append(words[idx])
            idx += 1
        if idx < len(words):
            idx += 1
        variables = ['VALUES'] + var_values

    assigned_vars = set()
    scan_idx = idx
    while scan_idx < len(words):
        w = words[scan_idx]
        if w == 'e801':
            if scan_idx + 1 < len(words):
                dest_addr = words[scan_idx + 1]
                dest_idx = int(dest_addr, 16)
                assigned_vars.add(dest_idx)
            scan_idx += 4
            if scan_idx - 1 < len(words) and words[scan_idx - 1] != '0000':
                scan_idx += 1
        elif w in ['efef', '1ff4', 'f8f8', 'f7f7', 'eaee', 'ebf4', 'e7e9', 'f6e9']:
            scan_idx += 2
        elif w in ['f0f0', 'f1f1', 'f2f2', 'f3f3']:
            scan_idx += 1
        elif w in ['fdfd', '04ed', 'eaed', 'ebed', 'eced', 'eaea']:
            scan_idx += 1
        else:
            scan_idx += 1

    def format_var(v_addr):
        if v_addr in inv_reserved_vars: return inv_reserved_vars[v_addr]
        if v_addr in inv_reserved_consts: return inv_reserved_consts[v_addr]
        v_idx = int(v_addr, 16)
        if v_idx > 0 and v_idx < len(variables):
            if v_idx not in assigned_vars:
                v_val = variables[v_idx]
                v_val_dec = int(v_val[2:4] + v_val[0:2], 16)
                return str(v_val_dec)
            return f"$var_{v_idx}"
        return f"VAR_{v_addr}"

    out_lines = []
    string_buffer = []
    def flush_string(as_ln=False):
        if string_buffer:
            s = "".join(string_buffer)
            if as_ln: out_lines.append(f"STRINGLN {s}")
            else: out_lines.append(f"STRING {s}")
            string_buffer.clear()

    while_gotos = {}
    for i in range(idx, len(words)):
        if words[i] == 'f8f8' and i + 1 < len(words):
            t_addr = words[i+1]
            t_dec = int(t_addr[2:4] + t_addr[0:2], 16)
            if t_dec < i: while_gotos[i] = t_dec

    block_stack = []
    
    while idx < len(words):
        w = words[idx]
        k = decode_word(w)
        is_char = (len(k) == 1 and w[2:4] in ['00', '02'])

        if string_buffer and not is_char:
            if w == '2800':
                flush_string(as_ln=True)
                idx += 1
                continue
            flush_string()
            
        if w == 'e801':
            dest = words[idx+1]
            src1 = words[idx+2]
            op_check = words[idx+3]
            if op_check == '0000':
                out_lines.append(f"{format_var(dest)} = {format_var(src1)}")
                idx += 4
            else:
                op = words[idx+4]
                op_sym = inv_op_map.get(op, "OP")
                out_lines.append(f"{format_var(dest)} = ({format_var(src1)} {op_sym} {format_var(op_check)})")
                idx += 5
        elif w == 'efef':
            cond = words[idx+1]
            bid = words[idx+2]
            if out_lines and out_lines[-1].startswith(f"{format_var(cond)} = "):
                cond_expr = out_lines.pop().split("=", 1)[1].strip()
            else:
                cond_expr = format_var(cond)
            if cond_expr == "FALSE":
                out_lines.append(f"IF FALSE THEN")
                block_stack.append(("FUNCTION", bid))
            else:
                out_lines.append(f"IF {cond_expr} THEN")
                block_stack.append(("IF", bid))
            idx += 3
        elif w == '1ff4':
            bid = words[idx+1]
            btype = block_stack.pop()[0] if block_stack else "IF"
            if btype == "FUNCTION": out_lines.append("END_FUNCTION")
            else: out_lines.append("END_IF")
            idx += 2
        elif w == 'f8f8':
            t_addr = words[idx+1]
            t_dec = int(t_addr[2:4] + t_addr[0:2], 16)
            if idx in while_gotos: out_lines.append(f"GOTO start_of_while_{t_dec}")
            else: out_lines.append(f"GOTO {t_dec}")
            idx += 2
        elif w == 'f7f7':
            t_addr = words[idx+1]
            t_dec = int(t_addr[2:4] + t_addr[0:2], 16)
            out_lines.append(f"FUNCTION_{t_dec}()")
            idx += 2
        elif w == 'fdfd':
            if out_lines and out_lines[-1].startswith("$var_f_ret = "):
                ret_val = out_lines.pop().split("=", 1)[1].strip()
                out_lines.append(f"RETURN {ret_val}")
            else: out_lines.append("RETURN")
            idx += 1
        elif w == 'fff8':
            key = words[idx+1]
            out_lines.append(f"HOLD {decode_word(key)}")
            idx += 2
        elif w == 'eee8':
            key = words[idx+1]
            if key == '0000': out_lines.append("RELEASE")
            else: out_lines.append(f"RELEASE {decode_word(key)}")
            idx += 2
        elif w == 'eaee':
            bid = words[idx+1]
            out_lines.append(f"BUTTON_DEF")
            idx += 2
        elif w == 'ebf4':
            bid = words[idx+1]
            out_lines.append(f"END_BUTTON")
            idx += 2
        elif w == 'e9e9':
            arg = words[idx+1]
            rmap = {"fa42": "RANDOM_LOWERCASE_LETTER", "fb42": "RANDOM_UPPERCASE_LETTER", "ff42": "RANDOM_LETTER", "fc42": "RANDOM_NUMBER", "fd42": "RANDOM_SPECIAL", "fe42": "RANDOM_CHAR"}
            if arg in rmap: out_lines.append(rmap[arg])
            else: out_lines.append(f"INJECT_VAR {format_var(arg)}")
            idx += 2
        elif w == 'e7e9':
            out_lines.append(f"DELAY {format_var(words[idx+1])}")
            idx += 2
        elif w == 'f6e9':
            out_lines.append(f"EXFIL {format_var(words[idx+1])}")
            idx += 2
        elif w in ['f0f0', 'f1f1', 'f2f2', 'f3f3']:
            mode = {"f0f0": "OFF", "f1f1": "HID", "f2f2": "STORAGE", "f3f3": "HID STORAGE"}[w]
            idx += 1
            attack_args = []
            while idx < len(words) and words[idx] != w:
                aw = words[idx]
                if aw == 'f5f5':
                    v = format_var(words[idx+1])
                    if v == format_var('f342'): attack_args.append("VID_RANDOM")
                    else: attack_args.append(f"VID_{v}")
                    idx += 2
                elif aw == 'f6f6':
                    v = format_var(words[idx+1])
                    if v == format_var('f342'): attack_args.append("PID_RANDOM")
                    else: attack_args.append(f"PID_{v}")
                    idx += 2
                elif aw == 'f9f9':
                    idx += 1
                    s = ""
                    while words[idx] != 'f9f9': s += chr(int(variables[int(words[idx][2:4]+words[idx][0:2], 16)][2:4]+variables[int(words[idx][2:4]+words[idx][0:2], 16)][0:2], 16)); idx += 1
                    attack_args.append(f"MAN_{s}")
                    idx += 1
                elif aw == 'fafa':
                    idx += 1
                    s = ""
                    while words[idx] != 'fafa': s += chr(int(variables[int(words[idx][2:4]+words[idx][0:2], 16)][2:4]+variables[int(words[idx][2:4]+words[idx][0:2], 16)][0:2], 16)); idx += 1
                    attack_args.append(f"PROD_{s}")
                    idx += 1
                elif aw == 'fbfb':
                    idx += 1
                    s = ""
                    while words[idx] != 'fbfb': s += chr(int(variables[int(words[idx][2:4]+words[idx][0:2], 16)][2:4]+variables[int(words[idx][2:4]+words[idx][0:2], 16)][0:2], 16)); idx += 1
                    attack_args.append(f"SERIAL_{s}")
                    idx += 1
                else:
                    idx += 1
            out_lines.append(f"ATTACKMODE {mode} {' '.join(attack_args)}")
            idx += 1
        elif w[0:2] == '00':
            d = int(w[2:4], 16)
            out_lines.append(f"DELAY {d}")
            idx += 1
        elif w in inv_builtins:
            out_lines.append(inv_builtins[w])
            idx += 1
        else:
            if is_char:
                string_buffer.append(k)
            else:
                out_lines.append(k)
            idx += 1

    flush_string()
    return "\n".join(out_lines)

def main():
    parser = argparse.ArgumentParser(description="Python CLI DuckyScript 3.0 Compiler")
    parser.add_argument("script_file", help="Path to input DuckyScript text file")
    parser.add_argument("output_file", help="Path to output binary payload file (e.g. inject.bin)")
    parser.add_argument("-l", "--lang", default="us", help="Keyboard layout name (default: us)")
    
    args = parser.parse_args()
    
    script_path = args.script_file
    output_path = args.output_file
    lang_name = args.lang
    
    # Try to load keyboard layout
    # First check in local directory, then in a relative languages/ folder
    script_dir = os.path.dirname(os.path.realpath(__file__))
    layout_paths = [
        os.path.join(script_dir, "languages", f"{lang_name}.json"),
        os.path.join(script_dir, f"{lang_name}.json"),
        f"{lang_name}.json"
    ]
    
    layout_data = None
    loaded_path = None
    for p in layout_paths:
        if os.path.exists(p):
            try:
                with open(p, "r", encoding="utf-8") as f:
                    layout_data = json.load(f)
                    loaded_path = p
                    break
            except Exception as e:
                print(f"Error loading layout file {p}: {e}", file=sys.stderr)
                
    if layout_data is None:
        print(f"Error: Keyboard layout for language '{lang_name}' could not be found.", file=sys.stderr)
        print("Checked paths:", file=sys.stderr)
        for p in layout_paths:
            print(f"  - {p}", file=sys.stderr)
        sys.exit(1)
        
    print(f"Loaded layout: {loaded_path}")
    
    # Check if input file is binary (inject.bin decompilation mode) or text (compilation mode)
    is_binary = False
    try:
        with open(script_path, "rb") as f:
            header = f.read(2)
            if header == b'\xe8\xe8' or script_path.endswith('.bin'):
                is_binary = True
    except Exception as e:
        print(f"Error reading file {script_path}: {e}", file=sys.stderr)
        sys.exit(1)

    if is_binary:
        print(f"Detected binary file {script_path}. Decompiling...")
        try:
            with open(script_path, "rb") as f:
                bin_data = f.read()
            text_data = decompile_payload(bin_data, layout_data)
            with open(output_path, "w", encoding="utf-8") as f:
                f.write(text_data)
            print(f"Successfully decompiled {script_path} to {output_path}")
        except Exception as e:
            print(f"Decompilation Failed: {e}", file=sys.stderr)
            sys.exit(1)
    else:
        print(f"Compiling {script_path}...")
        # Read duckyscript file
        try:
            with open(script_path, "r", encoding="utf-8", errors="ignore") as f:
                script_content = f.read()
        except Exception as e:
            print(f"Error reading script file {script_path}: {e}", file=sys.stderr)
            sys.exit(1)
            
        # Compile
        try:
            bin_data = compile_payload(script_content, layout_data)
        except Exception as e:
            print(f"Compilation Failed: {e}", file=sys.stderr)
            sys.exit(1)
            
        # Write output binary
        try:
            with open(output_path, "wb") as f:
                f.write(bin_data)
            print(f"Successfully compiled {script_path} to {output_path} ({len(bin_data)} Bytes)")
        except Exception as e:
            print(f"Error writing output file {output_path}: {e}", file=sys.stderr)
            sys.exit(1)

if __name__ == "__main__":
    main()
