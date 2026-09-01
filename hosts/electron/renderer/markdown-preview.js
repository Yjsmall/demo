import MarkdownIt from 'markdown-it';
import { katex } from '@mdit/plugin-katex';
import createDOMPurify from 'dompurify';
import 'katex/dist/katex.min.css';

const purifier = createDOMPurify(window);
const markdown = new MarkdownIt({
  html: false,
  linkify: true,
  breaks: false,
});
markdown.use(katex);

const defaultImageRule = markdown.renderer.rules.image;
markdown.renderer.rules.image = (tokens, index, options, environment, renderer) => {
  const sourceIndex = tokens[index].attrIndex('src');
  const source = sourceIndex >= 0 ? tokens[index].attrs[sourceIndex][1] : '';
  if (!/^reader-asset:[0-9a-f]{32}$/.test(source)) {
    return markdown.utils.escapeHtml(tokens[index].content || '');
  }
  return defaultImageRule(tokens, index, options, environment, renderer);
};

window.contextReaderMarkdown = Object.freeze({
  render(source) {
    const rendered = markdown.render(String(source ?? ''));
    return purifier.sanitize(rendered, {
      USE_PROFILES: { html: true },
      FORBID_TAGS: ['script', 'style', 'iframe', 'object', 'embed', 'form'],
      FORBID_ATTR: ['style', 'srcset'],
      ALLOWED_URI_REGEXP: /^(?:(?:https?|mailto):|reader-asset:[0-9a-f]{32}$)/i,
    });
  },
});
