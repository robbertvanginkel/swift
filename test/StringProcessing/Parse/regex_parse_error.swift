// RUN: %target-typecheck-verify-swift -enable-bare-slash-regex -disable-availability-checking
// REQUIRES: swift_in_compiler

_ = /(/ // expected-error {{expected ')'}}
_ = #/(/# // expected-error {{expected ')'}}

// FIXME: Should be 'group openings'
_ = /)/ // expected-error {{closing ')' does not balance any groups openings}}
_ = #/)/# // expected-error {{closing ')' does not balance any groups openings}}

_ = #/\\/''/ // expected-error {{unterminated regex literal}}
_ = #/\| // expected-error {{unterminated regex literal}}
_ = #// // expected-error {{unterminated regex literal}}

_ = #/xy // expected-error {{unterminated regex literal}}

_ = #/(?/# // expected-error {{expected group specifier}}
_ = #/(?'/# // expected-error {{expected group name}}
_ = #/(?'abc/# // expected-error {{expected '''}}
_ = #/(?'abc /# // expected-error {{expected '''}}

do {
  _ = #/(?'a
  // expected-error@-1 {{unterminated regex literal}}
  // expected-error@-2 {{cannot parse regular expression: expected '''}}
}

_ = #/\(?'abc/#

do {
  _ = /\
  /
  // expected-error@-2 {{unterminated regex literal}}
  // expected-error@-3 {{expected escape sequence}}
} // expected-error {{expected expression after operator}}

do {
  _ = #/\
  /#
  // expected-error@-2 {{unterminated regex literal}}
  // expected-error@-3 {{expected escape sequence}}
  // expected-error@-3 {{unterminated regex literal}}
  // expected-warning@-4 {{regular expression literal is unused}}
}

func foo<T>(_ x: T, _ y: T) {}
foo(#/(?/#, #/abc/#) // expected-error {{expected group specifier}}
foo(#/(?C/#, #/abc/#) // expected-error {{expected ')'}}

foo(#/(?'/#, #/abc/#) // expected-error {{expected group name}}
