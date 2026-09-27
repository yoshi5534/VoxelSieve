// A small Mustache subset for report templates. Only what a report needs: variables, sections,
// inverted sections and comments; no partials, lambdas or delimiter changes.

#include <optional>
#include <ranges>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "voxelsieve/report.hpp"

namespace voxelsieve {
namespace {

using Json = nlohmann::json;

std::string escapeHtml(std::string_view text) {
  std::string out;
  out.reserve(text.size());
  for (const char c : text) {
    switch (c) {
      case '&':
        out += "&amp;";
        break;
      case '<':
        out += "&lt;";
        break;
      case '>':
        out += "&gt;";
        break;
      case '"':
        out += "&quot;";
        break;
      case '\'':
        out += "&#39;";
        break;
      default:
        out += c;
    }
  }
  return out;
}

std::string_view trim(std::string_view text) {
  const auto first = text.find_first_not_of(" \t\r\n");
  if (first == std::string_view::npos) {
    return {};
  }
  const auto last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

/// Looks up a dotted name from the innermost context outwards, like Mustache: the first part is
/// searched on the stack, the rest inside the object found.
const Json* lookup(const std::vector<const Json*>& stack, std::string_view name) {
  if (name == ".") {
    return stack.back();
  }
  const auto dot = name.find('.');
  const std::string head(name.substr(0, dot));
  const Json* value = nullptr;
  for (const Json* context : std::ranges::reverse_view(stack)) {
    if (context->is_object()) {
      if (const auto found = context->find(head); found != context->end()) {
        value = &*found;
        break;
      }
    }
  }
  std::string_view rest = dot == std::string_view::npos ? std::string_view{} : name.substr(dot + 1);
  while (value != nullptr && !rest.empty()) {
    const auto next = rest.find('.');
    const std::string part(rest.substr(0, next));
    if (!value->is_object()) {
      return nullptr;
    }
    const auto found = value->find(part);
    value = found == value->end() ? nullptr : &*found;
    rest = next == std::string_view::npos ? std::string_view{} : rest.substr(next + 1);
  }
  return value;
}

bool truthy(const Json* value) {
  if (value == nullptr || value->is_null()) {
    return false;
  }
  if (value->is_boolean()) {
    return value->get<bool>();
  }
  if (value->is_array() || value->is_object() || value->is_string()) {
    return !value->empty() && !(value->is_string() && value->get<std::string>().empty());
  }
  return true;  // numbers, including 0, are shown
}

std::string toText(const Json* value) {
  if (value == nullptr || value->is_null()) {
    return {};
  }
  if (value->is_string()) {
    return value->get<std::string>();
  }
  return value->dump();
}

class Renderer {
 public:
  explicit Renderer(std::string_view text) : text_(text) {}

  std::string render(const Json& data) {
    std::vector<const Json*> stack{&data};
    std::string out;
    std::size_t pos = 0;
    renderUntil(pos, stack, out, {});
    return out;
  }

 private:
  struct Tag {
    char kind = 0;  // 0 variable, '{' raw, '#', '^', '/', '!'
    std::string name;
    std::size_t begin = 0;  // position of "{{"
    std::size_t end = 0;    // position after "}}"
  };

  [[nodiscard]] std::optional<Tag> nextTag(std::size_t pos) const {
    const auto open = text_.find("{{", pos);
    if (open == std::string_view::npos) {
      return std::nullopt;
    }
    Tag tag;
    tag.begin = open;
    std::size_t inner = open + 2;
    std::string_view close = "}}";
    if (inner < text_.size() && text_[inner] == '{') {
      tag.kind = '{';
      ++inner;
      close = "}}}";
    } else if (inner < text_.size() && (text_[inner] == '#' || text_[inner] == '^' ||
                                        text_[inner] == '/' || text_[inner] == '!')) {
      tag.kind = text_[inner];
      ++inner;
    }
    const auto end = text_.find(close, inner);
    if (end == std::string_view::npos) {
      throw std::runtime_error("Template: unclosed tag at offset " + std::to_string(open));
    }
    tag.name = std::string(trim(text_.substr(inner, end - inner)));
    tag.end = end + close.size();
    return tag;
  }

  /// Renders from `pos` until the closing tag of `section` (or the end for the top level), and
  /// leaves `pos` after that closing tag.
  void renderUntil(std::size_t& pos, std::vector<const Json*>& stack, std::string& out,
                   const std::string& section) {
    while (true) {
      const auto tag = nextTag(pos);
      if (!tag) {
        if (!section.empty()) {
          throw std::runtime_error("Template: section '" + section + "' is not closed");
        }
        out.append(text_.substr(pos));
        pos = text_.size();
        return;
      }
      out.append(text_.substr(pos, tag->begin - pos));
      pos = tag->end;
      switch (tag->kind) {
        case '!':
          break;
        case '/':
          if (tag->name != section) {
            throw std::runtime_error("Template: unexpected {{/" + tag->name + "}}" +
                                     (section.empty() ? "" : ", expected {{/" + section + "}}"));
          }
          return;
        case '#':
        case '^':
          renderSection(*tag, pos, stack, out);
          break;
        case '{':
          out += toText(lookup(stack, tag->name));
          break;
        default:
          out += escapeHtml(toText(lookup(stack, tag->name)));
      }
    }
  }

  void renderSection(const Tag& tag, std::size_t& pos, std::vector<const Json*>& stack,
                     std::string& out) {
    const Json* value = lookup(stack, tag.name);
    const std::size_t body = pos;
    const bool show = truthy(value);
    std::string discard;
    if (tag.kind == '^' || !show) {
      // Rendered once or not at all; a skipped body is still parsed to find its end.
      renderUntil(pos, stack, (tag.kind == '^') == show ? discard : out, tag.name);
      return;
    }
    if (value->is_array()) {
      for (const Json& item : *value) {
        pos = body;
        stack.push_back(&item);
        renderUntil(pos, stack, out, tag.name);
        stack.pop_back();
      }
      return;
    }
    stack.push_back(value);
    renderUntil(pos, stack, out, tag.name);
    stack.pop_back();
  }

  std::string_view text_;
};

}  // namespace

std::string renderTemplate(std::string_view text, const nlohmann::json& data) {
  return Renderer(text).render(data);
}

}  // namespace voxelsieve
