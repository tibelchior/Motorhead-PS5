#include <platform/frontend.hpp>
#include <platform/deck_controller.hpp>
#include <platform/deck_text_input.hpp>
#include <platform/deck_movie_input.hpp>
#include <platform/deck_bindings.hpp>
extern "C" bool mh_ps5_kb_active(void);
extern "C" void mh_ps5_kb_open(const char *initial, unsigned max_chars);
extern "C" int mh_ps5_kb_event(const SDL_Event *event, char *out, int capacity);

namespace {
// PS5: no keyboard, so X on a text row opens the on-screen keyboard (ps5_keyboard.cpp). Its result
// replaces the row's text: the old text is deleted, then the new one typed, as keyboard events.
void push_text(SDL_Window *window, const char *text) {
  static char buffers[8][64];
  static unsigned next = 0;
  char *slot = buffers[next++ % 8];
  SDL_strlcpy(slot, text, 64);
  SDL_Event e{};
  e.type = SDL_EVENT_TEXT_INPUT;
  e.text.windowID = SDL_GetWindowID(window);
  e.text.text = slot;
  SDL_PushEvent(&e);
}
void push_backspace(SDL_Window *window) {
  for (const bool down : {true, false}) {
    SDL_Event e{};
    e.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
    e.key.windowID = SDL_GetWindowID(window);
    e.key.key = SDLK_BACKSPACE;
    e.key.scancode = SDL_SCANCODE_BACKSPACE;
    e.key.down = down;
    SDL_PushEvent(&e);
  }
}
std::string row_text(const mh::ui::FrontEndState &state) {
  using mh::ui::PersonalOptionsField;
  if (state.screen() != mh::ui::FrontEndScreen::personal_options) return {};
  const auto &personal = state.personal_options_configuration();
  const auto row = state.personal_options_field();
  if (row == PersonalOptionsField::name) return personal.player_name;
  if (row == PersonalOptionsField::team) return personal.team_name;
  if (row >= PersonalOptionsField::short_key_0 && row <= PersonalOptionsField::short_key_9)
    return personal.short_keys[static_cast<std::size_t>(row) - static_cast<std::size_t>(PersonalOptionsField::short_key_0)];
  return {};
}
}  // namespace

namespace mh::platform {
void configure_frontend_platform() { DeckTextInput::configure(); }
struct FrontEndPlatform::Impl {
  SDL_Window *window;
  DeckController controller;
  DeckTextInput text;
  DeckMovieInput movies{controller};
  explicit Impl(SDL_Window *value):window(value) {}
};
FrontEndPlatform::FrontEndPlatform(SDL_Window *window):impl_(std::make_unique<Impl>(window)) {}
FrontEndPlatform::~FrontEndPlatform()=default;
bool FrontEndPlatform::skip_movie(SDL_Event &event) { return impl_->movies.handle(event); }
bool FrontEndPlatform::handle_event(SDL_Event &event,const mh::ui::FrontEndState &state,bool ready,
    const std::function<void(const std::string &)> &bind) {
  update(state,ready);
  DeckTextInput::trace(impl_->window,event,state);
  const bool capturing=ready && state.control_binding_capture_active();
  if(ready) {
    char typed[64]={};
    const int result=mh_ps5_kb_event(&event,typed,sizeof typed);
    if(result==2) {
      for(int i=0;i<64;++i) push_backspace(impl_->window);
      if(typed[0]) push_text(impl_->window,typed);
    }
    if(result) return true;
    if(!capturing && impl_->text.editing() && event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN &&
       event.gbutton.button==SDL_GAMEPAD_BUTTON_SOUTH) {
      mh_ps5_kb_open(row_text(state).c_str(),20);
      return true;
    }
  }
  if(capturing) {
    const auto binding=deck_binding_from_event(event);
    if(!binding.empty()) {
      bind(std::string(binding)); impl_->controller.handle(event,false); return true;
    }
  }
  if(event.type==SDL_EVENT_JOYSTICK_AXIS_MOTION || event.type==SDL_EVENT_JOYSTICK_BUTTON_DOWN) {
    const auto id=event.type==SDL_EVENT_JOYSTICK_AXIS_MOTION?event.jaxis.which:event.jbutton.which;
    if(SDL_IsGamepad(id)) return true;
    if(capturing && event.type==SDL_EVENT_JOYSTICK_AXIS_MOTION && event.jaxis.axis<3 &&
       (event.jaxis.value < -16000 || event.jaxis.value > 16000)) {
      const bool negative=event.jaxis.value<0;
      bind(event.jaxis.axis==0?(negative?"JoyLeft":"JoyRight"):
           event.jaxis.axis==1?(negative?"JoyUp":"JoyDown"):(negative?"JoyIn":"JoyOut"));
      return true;
    }
    if(capturing && event.type==SDL_EVENT_JOYSTICK_BUTTON_DOWN && event.jbutton.button<8) {
      bind("JoyButton"+std::to_string(event.jbutton.button+1));return true;
    }
  }
  const bool confirm=event.type==SDL_EVENT_GAMEPAD_BUTTON_DOWN && event.gbutton.button==SDL_GAMEPAD_BUTTON_SOUTH;
  impl_->controller.handle(event,!capturing);
  return ready && impl_->text.activate(impl_->window,event,confirm);
}
void FrontEndPlatform::update(const mh::ui::FrontEndState &state,bool ready) {
  impl_->text.sync(impl_->window,state,ready);
}
bool FrontEndPlatform::recover_after_focus(const SDL_Event &) { return false; }
struct RacePlatformInput::Impl { DeckController controller; };
RacePlatformInput::RacePlatformInput():impl_(std::make_unique<Impl>()) {}
RacePlatformInput::~RacePlatformInput()=default;
void RacePlatformInput::handle(SDL_Event &event,bool navigation) { impl_->controller.handle(event,navigation); }
}
